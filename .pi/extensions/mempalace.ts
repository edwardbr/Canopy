/**
 * MemPalace Extension for Canopy
 *
 * Integrates MemPalace into pi sessions for the Canopy project.
 * - Injects wake-up context on the first turn of each session
 * - Registers search, wakeup, and status tools for querying past decisions,
 *   bugs, architecture notes, and code context
 */

import type { ExtensionAPI } from "@earendil-works/pi-coding-agent";
import { Type } from "typebox";
import { execFile } from "node:child_process";
import { promisify } from "node:util";

const execFileAsync = promisify(execFile);

const MEMPALACE_BIN = "mempalace";
const DEFAULT_WING = "canopy_gitlab";
const MAX_CONTEXT_CHARS = 2000;

async function runMempalace(args: string[]): Promise<string> {
    try {
        const { stdout } = await execFileAsync(MEMPALACE_BIN, args, {
            timeout: 30_000,
            maxBuffer: 512 * 1024,
        });
        return stdout.trim();
    } catch (err: any) {
        if (err.killed) return "(error: mempalace timed out)";
        const stderr = err.stderr?.trim() || err.message;
        return `(error: mempalace: ${stderr})`;
    }
}

export default function mempalaceExtension(pi: ExtensionAPI) {
    // ── Tool: mempalace_search ──────────────────────────────────────────

    pi.registerTool({
        name: "mempalace_search",
        label: "MemPalace Search",
        description:
            "Search Canopy's MemPalace for past decisions, bugs, architecture notes, " +
            "and code context. Use this before making significant changes to check if " +
            "similar work was done before, to investigate known bugs, or to understand " +
            "why certain patterns exist in the codebase.",
        promptSnippet:
            "Search past Canopy decisions, bugs, and context via MemPalace",
        promptGuidelines: [
            "Use mempalace_search before making significant changes to check for past decisions, bugs, or architecture discussions.",
            "Use mempalace_search when investigating a bug to see if it was previously encountered and documented.",
            "Use mempalace_search to find context about specific files, patterns, or subsystems in the Canopy codebase.",
            "Search the 'Canopy' wing (--room decisions or --room bugs) for past decisions and known issues.",
        ],
        parameters: Type.Object({
            query: Type.String({
                description: "Search query (keywords, symbols, concepts)",
            }),
            wing: Type.Optional(
                Type.String({
                    description:
                        "Wing to search. 'canopy_gitlab' for code/docs (default), 'Canopy' for decisions/bugs.",
                }),
            ),
            room: Type.Optional(
                Type.String({
                    description:
                        "Room to narrow search. canopy_gitlab: c++, rust, documents, cmake, interfaces, generator. Canopy: decisions, bugs.",
                }),
            ),
        }),
        async execute(_toolCallId, params) {
            const wing = params.wing || DEFAULT_WING;
            const args = ["search", params.query, "--wing", wing];
            if (params.room) {
                args.push("--room", params.room);
            }

            const result = await runMempalace(args);

            return {
                content: [
                    {
                        type: "text",
                        text: result || "(no results found)",
                    },
                ],
                details: { wing, room: params.room, query: params.query },
            };
        },
    });

    // ── Tool: mempalace_wakeup ─────────────────────────────────────────

    pi.registerTool({
        name: "mempalace_wakeup",
        label: "MemPalace Wake-Up",
        description:
            "Get MemPalace wake-up context for Canopy — shows recent decisions, bugs, " +
            "and key story context. Use to reorient at session start or after major changes.",
        promptSnippet: "Show Canopy MemPalace wake-up context (decisions, bugs, key context)",
        promptGuidelines: [
            "Use mempalace_wakeup at the start of a session to get oriented on recent decisions and bugs.",
        ],
        parameters: Type.Object({}),
        async execute() {
            const codeWakeup = await runMempalace([
                "wake-up",
                "--wing",
                "canopy_gitlab",
            ]);
            const decisionsWakeup = await runMempalace([
                "wake-up",
                "--wing",
                "Canopy",
            ]);

            const text = [
                "## Canopy Codebase (canopy_gitlab)",
                codeWakeup.slice(0, MAX_CONTEXT_CHARS),
                "",
                "## Decisions & Bugs (Canopy)",
                decisionsWakeup.slice(0, MAX_CONTEXT_CHARS),
            ].join("\n");

            return {
                content: [{ type: "text", text }],
                details: {},
            };
        },
    });

    // ── Tool: mempalace_status ─────────────────────────────────────────

    pi.registerTool({
        name: "mempalace_status",
        label: "MemPalace Status",
        description:
            "Show MemPalace index status — wings, rooms, and drawer counts.",
        promptSnippet: "Show MemPalace index status",
        parameters: Type.Object({}),
        async execute() {
            const status = await runMempalace(["status"]);
            return {
                content: [{ type: "text", text: status }],
                details: {},
            };
        },
    });

    // ── Auto-inject wake-up on first turn of each session ──────────────

    let wakeupInjected = false;

    pi.on("session_start", () => {
        wakeupInjected = false;
    });

    pi.on("before_agent_start", async (event) => {
        if (wakeupInjected) return;
        wakeupInjected = true;

        try {
            const wakeup = await runMempalace(["wake-up", "--wing", "Canopy"]);

            // Trim to stay within reasonable context budget
            const trimmed =
                wakeup.length > MAX_CONTEXT_CHARS
                    ? wakeup.slice(0, MAX_CONTEXT_CHARS) +
                      "\n... (use mempalace_wakeup or mempalace_search for full context)"
                    : wakeup;

            return {
                message: {
                    customType: "mempalace-wakeup",
                    content: `**MemPalace: Recent Canopy Decisions & Bugs**\n\n${trimmed}`,
                    display: true,
                },
            };
        } catch {
            // Silently skip if mempalace isn't available
            return;
        }
    });
}
