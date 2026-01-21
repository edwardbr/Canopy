/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rpc/rpc.h>
#include <rpc/telemetry/animation_telemetry_service.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace
{
    constexpr const char* kAnimationStyles = R"CSS(
body {
    font-family: "Segoe UI", Roboto, sans-serif;
    margin: 0;
    background-color: #0f172a;
    color: #e2e8f0;
    display: flex;
    flex-direction: column;
    height: 100vh;
    overflow: hidden;
}

.header {
    padding: 16px 24px 8px 24px;
    background: linear-gradient(90deg, #1e293b 0%, #0f172a 80%);
    box-shadow: 0 2px 4px rgba(15, 23, 42, 0.6);
}

.header h1 {
    margin: 0;
    font-size: 22px;
    letter-spacing: 0.02em;
}

.header .subtitle {
    margin-top: 4px;
    font-size: 14px;
    color: #94a3b8;
}

.controls {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 12px 24px;
    background-color: #111c3a;
    border-bottom: 1px solid rgba(148, 163, 184, 0.1);
    flex-wrap: wrap;
}

.controls button {
    background-color: #2563eb;
    color: #e2e8f0;
    border: none;
    padding: 6px 14px;
    border-radius: 6px;
    font-size: 14px;
    cursor: pointer;
    transition: background-color 0.15s ease-in-out, transform 0.15s ease-in-out;
}

.controls button:hover {
    background-color: #1d4ed8;
    transform: translateY(-1px);
}

.controls button:disabled {
    opacity: 0.5;
    cursor: default;
    transform: none;
}

.controls .speed-control {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    font-size: 13px;
    color: #cbd5f5;
}

.controls select {
    background-color: #1e293b;
    color: #e2e8f0;
    border: 1px solid rgba(148, 163, 184, 0.4);
    border-radius: 6px;
    padding: 4px 6px;
    font-size: 13px;
}

.controls input[type="range"] {
    width: 320px;
}

.controls .time-label {
    font-variant-numeric: tabular-nums;
    min-width: 80px;
    text-align: right;
}

.type-filters {
    display: inline-flex;
    align-items: center;
    gap: 10px;
    flex-wrap: wrap;
}

.type-filters .filter-title {
    font-size: 12px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    color: #94a3b8;
}

.type-filters .filter-item {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    font-size: 12px;
    background-color: rgba(30, 58, 138, 0.35);
    padding: 4px 8px;
    border-radius: 6px;
}

.type-filters .filter-item input[type="checkbox"] {
    accent-color: #2563eb;
    width: 14px;
    height: 14px;
}

#main-layout {
    display: flex;
    flex: 1 1 auto;
    min-height: 0;
    width: 100%;
    overflow: hidden;
}

#viz-container {
    position: relative;
    flex: 1 1 auto;
    min-height: 480px;
    background-color: #0b1120;
    overflow: hidden;
}

#viz-container .overlay-hint {
    position: absolute;
    top: 12px;
    right: 16px;
    background: rgba(15, 23, 42, 0.8);
    padding: 6px 12px;
    border-radius: 6px;
    font-size: 12px;
    color: #cbd5f5;
}

#event-panel {
    display: flex;
    flex-direction: column;
    background-color: #0f172a;
    border-left: 1px solid rgba(148, 163, 184, 0.1);
    width: 320px;
    max-width: 40vw;
    padding: 12px 24px 24px 24px;
    transition: width 0.2s ease-in-out, opacity 0.2s ease-in-out, padding 0.2s ease-in-out;
    overflow: hidden;
    position: relative;
    z-index: 10;
    min-height: 0;
}

#event-panel h2 {
    margin: 0;
    font-size: 16px;
    color: #cbd5f5;
}

.event-log-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    margin-bottom: 12px;
}

.event-log-toggle {
    background-color: #1e293b;
    color: #e2e8f0;
    border: 1px solid rgba(148, 163, 184, 0.4);
    padding: 4px 10px;
    border-radius: 6px;
    font-size: 12px;
    cursor: pointer;
    transition: background-color 0.15s ease-in-out, transform 0.15s ease-in-out;
}

.event-log-toggle:hover {
    background-color: #334155;
    transform: translateY(-1px);
}

#event-log-body {
    flex: 1;
    overflow-y: auto;
    transition: max-height 0.2s ease-in-out, opacity 0.2s ease-in-out, padding 0.2s ease-in-out;
    min-height: 0;
}

.event-log-body-collapsed {
    flex: 0 !important;
    max-height: 0 !important;
    padding-top: 0 !important;
    padding-bottom: 0 !important;
    opacity: 0;
    overflow: hidden !important;
}

.event-panel-collapsed {
    width: 0 !important;
    padding: 12px 0 !important;
    opacity: 0;
    pointer-events: none;
    border-left: none;
}

#event-log {
    list-style: none;
    margin: 0;
    padding: 0;
    display: block;
}

#event-log li {
    padding: 8px 10px;
    border-radius: 6px;
    background: rgba(15, 23, 42, 0.75);
    box-shadow: inset 0 0 0 1px rgba(37, 99, 235, 0.2);
    font-size: 12px;
    margin-bottom: 6px;
}

#event-log li .timestamp {
    color: #94a3b8;
    font-variant-numeric: tabular-nums;
}

svg {
    display: block;
}

/* Trunk lines connecting zones */
.trunk-line {
    stroke: #2c3e50;
    stroke-width: 12px;
    stroke-linecap: round;
    opacity: 0.8;
}

.zone-bg {
    fill: #111113;
    stroke: #00d2ff;
    stroke-width: 2;
}

.service-box {
    fill: #0984e3;
    stroke: #74b9ff;
}

.transport-box {
    fill: #d35400;
    stroke: #ff8c61;
}

.transport-label {
    font-size: 9px;
    fill: #ffffff;
    font-weight: 600;
    pointer-events: none;
}

.transport-detail {
    font-size: 9px;
    fill: #fef3c7;
    pointer-events: none;
}

.pass-box {
    fill: #6c5ce7;
    stroke: #a29bfe;
    stroke-width: 2;
}

.pass-label {
    font-size: 9px;
    fill: #ffffff;
    font-weight: 600;
    pointer-events: none;
}

.pass-detail {
    font-size: 9px;
    fill: #e9d5ff;
    pointer-events: none;
}

.wire {
    stroke: #ffffff;
    stroke-width: 1.2;
    stroke-dasharray: 4, 2;
    fill: none;
    opacity: 0.3;
}

.wire.routing {
    stroke: #a29bfe;
    stroke-width: 2;
    stroke-dasharray: none;
    opacity: 1;
}

.label {
    font-size: 10px;
    fill: white;
    text-anchor: middle;
    font-weight: bold;
    pointer-events: none;
}
)CSS";

    constexpr const char* kAnimationScriptPart1 = R"JS(
(function() {
    const width = 1280;
    const height = 720;
    const levelSpacing = 120;
    const paddingBottom = 80;
    const maxLogEntries = 60;
    const epsilon = 1e-6;
    const transportLineHeight = 12;
    const transportBoxPaddingX = 8;
    const transportBoxPaddingY = 6;
    const transportMinWidth = 60;
    const transportMinHeight = 30;
    const transportMaxWidth = 320;
    const passthroughLineHeight = 12;
    const passthroughBoxPaddingX = 8;
    const passthroughBoxPaddingY = 6;
    const passthroughMinWidth = 80;
    const passthroughMinHeight = 30;
    const passthroughMaxWidth = 240;

    const palette = {
        zone: '#38bdf8',
        service_proxy: '#a855f7',
        object_proxy: '#fb7185',
        interface_proxy: '#fbbf24',
        impl: '#22d3ee',
        stub: '#4ade80',
        activity: '#f97316',
        passthrough: '#ec4899',
        transport: '#10b981'
    };

    const levelByType = {
        zone: 0,
        transport: 0.5,
        service_proxy: 1,
        passthrough: 1,
        object_proxy: 2,
        stub: 2,
        impl: 3,
        interface_proxy: 3,
        activity: 4
    };

    const maxNodeLevel = Math.max(...Object.values(levelByType));

    const nodeRadiusByType = {
        zone: 28,
        transport: 12,
        service_proxy: 18,
        passthrough: 16,
        object_proxy: 16,
        stub: 16,
        impl: 14,
        interface_proxy: 14,
        activity: 10
    };

    const linkStrengthByType = {
        contains: 0.8,
        route: 0.4,
        activity: 0.2,
        impl_route: 0.5,
        channel_route: 0.5,
        passthrough_link: 0.6,
        transport_link: 1.0
    };

    const zoneBoxWidth = 260;
    const zoneBoxHeight = 280;
    const zoneLabelOffset = -zoneBoxHeight / 2 + 18;
    const zoneColumnSpacing = zoneBoxWidth + 160;
    let zoneVerticalSpacing = zoneBoxHeight + 160;
    let canvasWidth = width;
    let canvasHeight = height;

    const nodes = new Map();
    const links = new Map();
    const zoneLayout = new Map();
    const zoneHierarchy = new Map();
    const zoneChildren = new Set();

    const simulation = d3.forceSimulation()
        .alphaDecay(0.02)  // Much slower decay for smoother animation
        .velocityDecay(0.8)  // Higher velocity decay to reduce oscillations
        .force('link', d3.forceLink()
            .id((d) => d.id)
            .distance((d) => 100 + (levelByType[d.type] || 0) * 20)
            .strength((d) => linkStrengthByType[d.type] || 0.2))
        .force('charge', d3.forceManyBody().strength(-160))
        .force('collide', d3.forceCollide().radius((d) => nodeRadiusByType[d.type] + 12))
        .force('x', d3.forceX().x((d) => targetX(d)).strength(0.05))  // Reduced strength for gentler positioning
        .force('y', d3.forceY().y((d) => targetY(d)).strength(0.25))  // Reduced strength for gentler positioning
        .stop();

    // Tree layout structures for hierarchical visualization
    const zones = {};  // zone_id -> { id, name, parentId, transports[], passthroughs[], children[], width, height }
    const adjacencyList = {};  // zone_id -> Set of adjacent zone_ids (for BFS pathfinding)
    const PortRegistry = {};  // "zoneId:adjId" -> { relX, relY, absX, absY }
    const transportRefState = new Map();  // transportId -> { alive, createdAt, deletedAt, pairs: Map }
    const transportAuditState = { completed: false, lastEventTimestamp: 0 };
    const transportErrors = new Set();  // Set of transportIds with negative ref count errors
    const ADD_REF_OPTIMISTIC = 4;
    const RELEASE_OPTIMISTIC = 1;

    const implByAddress = new Map();
    const objectToImpl = new Map();
    const interfaceToImplLink = new Map();
    const stubByObject = new Map();
    const interfaceProxyIndex = new Map();
    const interfaceProxyKeyById = new Map();
    const proxyLinkIndex = new Map();
    const linkUsage = new Map();
    const activeZones = new Set([1]);
    const nodeTypeFilters = [
        { key: 'service_proxy', label: 'Service Proxies', defaultVisible: false },
        { key: 'object_proxy', label: 'Object Proxies', defaultVisible: false },
        { key: 'interface_proxy', label: 'Interface Proxies', defaultVisible: true },
        { key: 'stub', label: 'Stubs', defaultVisible: false },
        { key: 'impl', label: 'Implementations', defaultVisible: true },
        { key: 'activity', label: 'Activity Pulses', defaultVisible: false },
        { key: 'passthrough', label: 'Passthroughs', defaultVisible: false },
        { key: 'transport', label: 'Transports', defaultVisible: true }
    ];
    const typeVisibility = new Map();
    nodeTypeFilters.forEach((filter) => typeVisibility.set(filter.key, filter.defaultVisible));
    const zoneAliases = new Map();
    let primaryZoneId = null;

    // Build zoneMetadata from events
    const zoneMetadata = {};
    events.forEach(e => {
        if (e.type === 'zone_creation' || e.type === 'service_creation') {
            const zoneId = e.data.zone_id || e.data.zone;
            const parentZone = e.data.parent_zone_id || e.data.parentZone;
            const name = e.data.name || e.data.serviceName;
            if (zoneId !== undefined) {
                if (!zoneMetadata[zoneId]) {
                    zoneMetadata[zoneId] = {};
                }
                if (name) zoneMetadata[zoneId].name = name;
                if (parentZone !== undefined) zoneMetadata[zoneId].parent = parentZone;
            }
        }
    });

    function normalizeZoneNumber(zoneNumber) {
        if (zoneNumber === undefined || zoneNumber === null || Number.isNaN(zoneNumber)) {
            return primaryZoneId || 1;
        }
        if (zoneNumber === 0) {
            return primaryZoneId || 1;
        }
        return zoneNumber;
    }

    function noteZoneAlias(zoneNumber, name) {
        if (!name) {
            return;
        }
        if (!zoneAliases.has(zoneNumber)) {
            zoneAliases.set(zoneNumber, new Set());
        }
        const aliases = zoneAliases.get(zoneNumber);
        const previousSize = aliases.size;
        aliases.add(name);
        if (aliases.size !== previousSize && nodes.has(`zone-${zoneNumber}`)) {
            const zoneNode = nodes.get(`zone-${zoneNumber}`);
            if (zoneNode) {
                zoneNode.label = formatZoneLabel(zoneNumber);
            }
        }
    }

    function formatZoneLabel(zoneNumber) {
        const aliases = Array.from(zoneAliases.get(zoneNumber) || []);
        if (aliases.length === 0) {
            return `Zone ${zoneNumber}`;
        }
        const [primary, ...rest] = aliases;
        if (rest.length === 0) {
            return primary;
        }
        return `${primary} (aka ${rest.join(', ')})`;
    }

    function isNodeVisible(node) {
        if (!node) {
            return false;
        }
        if (node.type === 'zone') {
            return true;
        }
        if (!typeVisibility.has(node.type)) {
            return true;
        }
        return typeVisibility.get(node.type);
    }

    function resolveNodeId(endpoint) {
        if (!endpoint) {
            return null;
        }
        if (typeof endpoint === 'object') {
            return endpoint.id;
        }
        return endpoint;
    }

    function toZoneId(value) {
        if (value === undefined || value === null) {
            return null;
        }
        const numeric = Number(value);
        if (!Number.isFinite(numeric)) {
            return null;
        }
        return `zone-${numeric}`;
    }

    function ensureZoneNode(zoneNumberRaw) {
        const zoneNumber = normalizeZoneNumber(zoneNumberRaw);
        const zoneId = toZoneId(zoneNumber);
        if (!zoneId) {
            return null;
        }
        if (!nodes.has(zoneId)) {
            if (!activeZones.has(zoneNumber)) {
                return null;
            }
            const metadata = zoneMetadata[zoneNumber] || {};
            // Don't normalize parent=0 (root zones), keep it as-is for hierarchy
            const parentNumber = metadata.parent !== undefined ? metadata.parent : null;
            let parentZoneId = toZoneId(parentNumber);
            if (parentNumber === undefined || parentNumber === null || parentNumber === zoneNumber) {
                parentZoneId = null;
            }
            // Recursively ensure parent exists, but skip zone 0 (implicit root)
            if (parentZoneId && !nodes.has(parentZoneId) && parentNumber !== 0) {
                ensureZoneNode(parentNumber);
            }
            const initialLabel = formatZoneLabel(zoneNumber);
            const node = {
                id: zoneId,
                type: 'zone',
                label: initialLabel,
                zone: zoneNumber,
                parent: parentZoneId,
                refCount: 0
            };
            nodes.set(zoneId, node);

            // Also populate zones{} for tree layout
            if (!zones[zoneNumber]) {
                zones[zoneNumber] = {
                    id: zoneNumber,
                    name: metadata.name || initialLabel,
                    parentId: parentNumber || 0,
                    transports: [],
                    passthroughs: [],
                    children: [],
                    width: 260,
                    height: 140
                };

                // Initialize adjacency list
                if (!adjacencyList[zoneNumber]) {
                    adjacencyList[zoneNumber] = new Set();
                }
            } else if (parentNumber !== undefined && parentNumber !== null) {
                // Update parent if we're providing a more specific parent
                const oldParentId = zones[zoneNumber].parentId;
                if (oldParentId === 0 && parentNumber !== 0) {
                    // Ensure parent zone exists
                    if (!zones[parentNumber]) {
                        zones[parentNumber] = {
                            id: parentNumber,
                            name: `Zone ${parentNumber}`,
                            parentId: 0,
                            transports: [],
                            passthroughs: [],
                            children: [],
                            width: 260,
                            height: 140
                        };
                    }
                    // Update parent
                    zones[zoneNumber].parentId = parentNumber;
                }
            }

            // Add this zone to its parent's children array
            const currentParent = zones[zoneNumber].parentId;
            if (currentParent && currentParent !== 0) {
                // Ensure parent exists
                if (!zones[currentParent]) {
                    zones[currentParent] = {
                        id: currentParent,
                        name: `Zone ${currentParent}`,
                        parentId: 0,
                        transports: [],
                        passthroughs: [],
                        children: [],
                        width: 260,
                        height: 140
                    };
                }
                if (!zones[currentParent].children.includes(zoneNumber)) {
                    zones[currentParent].children.push(zoneNumber);
                }
            }

            if (parentZoneId && nodes.has(parentZoneId)) {
                const linkId = `zone-link-${parentZoneId}-${zoneId}`;
                if (!links.has(linkId)) {
                    links.set(linkId, {
                        id: linkId,
                        source: parentZoneId,
                        target: zoneId,
                        type: 'contains'
                    });
                }
            }
            recomputeZoneLayout();
        } else {
            const metadata = zoneMetadata[zoneNumber] || {};
            const node = nodes.get(zoneId);
            if (node) {
                if (metadata.name) {
                    noteZoneAlias(zoneNumber, metadata.name);
                }
                node.label = formatZoneLabel(zoneNumber);
            }
        }
        return zoneId;
    }

    // BFS pathfinding for passthrough routing
    function findNextHop(startNode, targetNode) {
        if (startNode === targetNode) return startNode;
        let queue = [[startNode]];
        let visited = new Set([startNode]);

        while (queue.length > 0) {
            let path = queue.shift();
            let node = path[path.length - 1];

            if (node === targetNode) {
                return path[1];  // Return second node in path (next hop)
            }

            const neighbors = adjacencyList[node];
            if (neighbors) {
                for (let neighbor of neighbors) {
                    if (!visited.has(neighbor)) {
                        visited.add(neighbor);
                        queue.push([...path, neighbor]);
                    }
                }
            }
        }

        return null;  // No path found
    }

    function recomputeZoneLayout() {
        zoneLayout.clear();
        const zoneNodes = Array.from(nodes.values()).filter((node) => node.type === 'zone');
        if (zoneNodes.length === 0) {
            return;
        }
        const childrenByParent = new Map();
        const roots = [];
        zoneNodes.forEach((node) => {
            const parentId = node.parent && node.parent !== node.id ? node.parent : null;
            if (parentId && nodes.has(parentId)) {
                if (!childrenByParent.has(parentId)) {
                    childrenByParent.set(parentId, []);
                }
                childrenByParent.get(parentId).push(node);
            } else {
                roots.push(node);
            }
        });
        roots.sort((a, b) => (a.zone || 0) - (b.zone || 0));
        let currentColumn = 0;
        const seen = new Set();

        function assign(node, depth) {
            if (!node || seen.has(node.id)) {
                return;
            }
            seen.add(node.id);
            const children = (childrenByParent.get(node.id) || [])
                .slice()
                .sort((a, b) => (a.zone || 0) - (b.zone || 0));
            if (children.length === 0) {
                const order = currentColumn++;
                zoneLayout.set(node.id, { depth, order });
            } else {
                children.forEach((child) => assign(child, depth + 1));
                const childOrders = children
                    .map((child) => zoneLayout.get(child.id)?.order)
                    .filter((value) => value !== undefined);
                const order = childOrders.length > 0
                    ? childOrders.reduce((sum, value) => sum + value, 0) / childOrders.length
                    : currentColumn++;
                zoneLayout.set(node.id, { depth, order });
            }
        }

        roots.forEach((root) => assign(root, 0));

        if (zoneLayout.size === 0) {
            return;
        }

        const maxDepth = Math.max(...Array.from(zoneLayout.values()).map((layout) => layout.depth || 0));
        const baseY = canvasHeight - paddingBottom;
        const minY = zoneBoxHeight / 2 + 80;
        if (maxDepth > 0) {
            const available = Math.max(baseY - minY, zoneBoxHeight);
            const rawSpacing = available / Math.max(maxDepth, 1);
            const minSpacing = Math.max(zoneBoxHeight * 0.6, 140);
            const maxSpacing = zoneBoxHeight + 240;
            let spacing = rawSpacing;
            if (spacing < minSpacing) {
                spacing = Math.min(minSpacing, available / Math.max(maxDepth, 1));
            }
            spacing = Math.min(spacing, maxSpacing);
            if (spacing * maxDepth > available + 1) {
                spacing = rawSpacing;
            }
            zoneVerticalSpacing = Math.max(spacing, zoneBoxHeight * 0.75);
        } else {
            zoneVerticalSpacing = zoneBoxHeight + 160;
        }

        const orders = Array.from(zoneLayout.values()).map((layout) => layout.order);
        const minOrder = Math.min(...orders);
        const orderOffset = minOrder < 0 ? -minOrder : 0;

        zoneLayout.forEach((layout, zoneId) => {
            const adjustedOrder = layout.order + orderOffset;
            const x = 160 + adjustedOrder * zoneColumnSpacing;
            const y = canvasHeight - paddingBottom - layout.depth * zoneVerticalSpacing;
            layout.x = x;
            layout.y = y;
            const zoneNode = nodes.get(zoneId);
            if (zoneNode) {
                zoneNode.fx = x;
                zoneNode.fy = y;
                zoneNode.x = x;
                zoneNode.y = y;
            }
        });
    }
    const hasDuration = totalDuration > epsilon;
    const desiredPlaybackSeconds = Math.max(events.length * 0.5, 8);
    const fallbackStep = desiredPlaybackSeconds / Math.max(events.length + 1, 2);
    const timeScale = hasDuration ? Math.max(desiredPlaybackSeconds / totalDuration, 1) : 1;
    const nominalDisplayDuration = hasDuration ? totalDuration * timeScale : desiredPlaybackSeconds;
    const eventDisplayTimes = hasDuration
        ? events.map((evt) => evt.timestamp * timeScale)
        : events.map((_, index) => (index + 1) * fallbackStep);
    const displayDuration = eventDisplayTimes.length > 0
        ? Math.max(eventDisplayTimes[eventDisplayTimes.length - 1], nominalDisplayDuration)
        : nominalDisplayDuration;
    ensureZoneNode(0);
    let processedIndex = 0;
    let processedDisplayTime = 0;
    let currentTime = 0;
    let timer = null;
    let playing = false;
    let playbackSpeed = 5;

    const vizContainer = d3.select('#viz-container');

    const svgRoot = vizContainer
        .append('svg')
        .attr('class', 'telemetry-svg');

    function resizeSvg() {
        const node = vizContainer.node();
        if (!node) {
            return;
        }
        const bounds = node.getBoundingClientRect();
        canvasWidth = Math.max(bounds.width, 640);
        canvasHeight = Math.max(bounds.height, 480);
        svgRoot
            .attr('width', canvasWidth)
            .attr('height', canvasHeight);
    }

    const g = svgRoot.append('g');  // Main container for tree layout

    svgRoot.call(d3.zoom()
        .on('zoom', (event) => {
            g.attr('transform', event.transform);
        }));

    const timelineSlider = document.getElementById('timeline-slider');
    const timeLabel = document.getElementById('time-display');
    const startButton = document.getElementById('start-button');
    const stopButton = document.getElementById('stop-button');
    const resetButton = document.getElementById('reset-button');
    const speedSelect = document.getElementById('speed-select');
    const eventLog = document.getElementById('event-log');
    const eventLogBody = document.getElementById('event-log-body');
    const showLogCheckbox = document.getElementById('show-log-checkbox');
    const typeFiltersContainer = document.getElementById('type-filters');
    const eventPanel = document.getElementById('event-panel');
    let logCollapsed = false;

    if (typeFiltersContainer) {
        nodeTypeFilters.forEach((filter) => {
            const label = document.createElement('label');
            label.className = 'filter-item';
            const checkbox = document.createElement('input');
            checkbox.type = 'checkbox';
            checkbox.id = `filter-${filter.key}`;
            checkbox.checked = typeVisibility.get(filter.key);
            checkbox.dataset.typeKey = filter.key;
            const caption = document.createElement('span');
            caption.textContent = filter.label;
            label.appendChild(checkbox);
            label.appendChild(caption);
            typeFiltersContainer.appendChild(label);
            checkbox.addEventListener('change', () => {
                typeVisibility.set(filter.key, checkbox.checked);
                rebuildVisualization();
            });
        });
    }


    const sliderMax = Math.max(displayDuration, 0.001);
    timelineSlider.max = sliderMax.toFixed(3);
    timelineSlider.step = Math.max(sliderMax / 200, 0.001).toFixed(3);

    startButton.addEventListener('click', () => startPlayback());
    stopButton.addEventListener('click', () => stopPlayback());
    resetButton.addEventListener('click', () => {
        stopPlayback();
        resetState();
        setCurrentTime(0);
    });

    timelineSlider.addEventListener('input', (event) => {
        const target = parseFloat(event.target.value);
        seekTo(target);
    });

    if (speedSelect) {
        speedSelect.addEventListener('change', (event) => {
            const value = parseFloat(event.target.value);
            if (Number.isFinite(value) && value > 0) {
                playbackSpeed = value;
            }
        });
    }

    if (showLogCheckbox && eventLogBody && eventPanel) {
        showLogCheckbox.addEventListener('change', () => {
            logCollapsed = !showLogCheckbox.checked;
            if (logCollapsed) {
                eventLogBody.classList.add('event-log-body-collapsed');
                eventPanel.classList.add('event-panel-collapsed');
            } else {
                eventLogBody.classList.remove('event-log-body-collapsed');
                eventPanel.classList.remove('event-panel-collapsed');
            }
            resizeSvg();
            rebuildVisualization();
        });
    }

    // TODO: Re-enable hover tooltips
    // svgRoot.on('mousemove', (event) => {
    //     const [mx, my] = d3.pointer(event);
    //     const zone = findClosestZone(mx, my);
    //     if (zone) {
    //         hoverTooltip
    //             .style('opacity', 1)
    //             .html(makeTooltip(zone))
    //             .style('right', '16px')
    //             .style('top', '12px');
    //     } else {
    //         hoverTooltip.style('opacity', 0);
    //     }
    // });

    // svgRoot.on('mouseleave', () => hoverTooltip.style('opacity', 0));

    resizeSvg();
    rebuildVisualization();

    window.addEventListener('resize', () => {
        resizeSvg();
        rebuildVisualization();
    });
    renderLegend();

    function targetY(node) {
        if (node.type === 'zone') {
            const layout = zoneLayout.get(node.id);
            if (layout) {
                return layout.y;
            }
            return canvasHeight - paddingBottom;
        }
        if (node.type === 'transport' && node.parent && node.adjacentZoneId) {
            const parentZone = nodes.get(node.parent);
            const adjacentZone = nodes.get(node.adjacentZoneId);
            if (parentZone && adjacentZone) {
                const x1 = parentZone.x ?? targetX(parentZone);
                const x2 = adjacentZone.x ?? targetX(adjacentZone);
                const y1 = parentZone.y ?? targetY(parentZone);
                const y2 = adjacentZone.y ?? targetY(adjacentZone);
                const dx = Math.abs(x2 - x1);
                const dy = Math.abs(y2 - y1);
                if (dx > dy) {
                    return y1;
                } else {
                    if (y1 < y2) {
                        return y1 + zoneBoxHeight / 2;
                    } else {
                        return y1 - zoneBoxHeight / 2;
                    }
                }
            }
        }
        const level = levelByType[node.type] ?? 0;
        if (node.parent && nodes.has(node.parent)) {
            const parent = nodes.get(node.parent);
            const parentY = parent.y ?? targetY(parent);
            const usableHeight = zoneBoxHeight - 160;
            const step = Math.max(usableHeight / Math.max(maxNodeLevel || 1, 1), 40);
            const baseY = parentY - zoneBoxHeight / 2 + 80;
            return baseY + level * step;
        }
        return canvasHeight - paddingBottom - level * levelSpacing;
    }

    function targetX(node) {
        if (node.type === 'zone') {
            const layout = zoneLayout.get(node.id);
            if (layout) {
                return layout.x;
            }
            return 160;
        }
        if (node.type === 'transport' && node.parent && node.adjacentZoneId) {
            const parentZone = nodes.get(node.parent);
            const adjacentZone = nodes.get(node.adjacentZoneId);
            if (parentZone && adjacentZone) {
                const x1 = parentZone.x ?? targetX(parentZone);
                const x2 = adjacentZone.x ?? targetX(adjacentZone);
                const y1 = parentZone.y ?? targetY(parentZone);
                const y2 = adjacentZone.y ?? targetY(adjacentZone);
                const dx = Math.abs(x2 - x1);
                const dy = Math.abs(y2 - y1);
                if (dx > dy) {
                    if (x1 < x2) {
                        return x1 + zoneBoxWidth / 2;
                    } else {
                        return x1 - zoneBoxWidth / 2;
                    }
                } else {
                    return x1;
                }
            }
        }
        if (node.parent && nodes.has(node.parent)) {
            const parent = nodes.get(node.parent);
            return parent.x ?? targetX(parent);
        }
        return canvasWidth / 2;
    }

    function startPlayback() {
        if (playing || events.length === 0) {
            return;
        }
        playing = true;
        const start = performance.now();
        let last = start;
        timer = d3.timer((now) => {
            const deltaMs = now - last;
            last = now;
            const deltaSeconds = (deltaMs / 1000) * playbackSpeed;
            if (deltaSeconds <= 0) {
                return;
            }
            const proposed = currentTime + deltaSeconds;
            if (proposed >= displayDuration - epsilon) {
                seekTo(displayDuration);
                stopPlayback();
                return;
            }
            seekTo(proposed);
        });
        updateButtons();
    }

    function stopPlayback() {
        if (!playing) {
            return;
        }
        playing = false;
        if (timer) {
            timer.stop();
            timer = null;
        }
        updateButtons();
    }

    function updateButtons() {
        startButton.disabled = playing || events.length == 0;
        stopButton.disabled = !playing;
    }

    function clearObject(obj) {
        Object.keys(obj).forEach((key) => delete obj[key]);
    }

    function removeZoneFromLayout(zoneNumber) {
        const zone = zones[zoneNumber];
        if (!zone) {
            return;
        }
        if (zone.parentId && zones[zone.parentId]) {
            zones[zone.parentId].children = zones[zone.parentId].children.filter((id) => id !== zoneNumber);
        }
        Object.values(zones).forEach((entry) => {
            entry.children = entry.children.filter((id) => id !== zoneNumber);
            entry.transports = entry.transports.filter((t) => t.adjId !== zoneNumber);
            entry.passthroughs = entry.passthroughs.filter((p) => p.fwd !== zoneNumber && p.rev !== zoneNumber);
        });
        if (adjacencyList[zoneNumber]) {
            adjacencyList[zoneNumber].forEach((neighbor) => {
                if (adjacencyList[neighbor]) {
                    adjacencyList[neighbor].delete(zoneNumber);
                    if (adjacencyList[neighbor].size === 0) {
                        delete adjacencyList[neighbor];
                    }
                }
            });
            delete adjacencyList[zoneNumber];
        }
        delete zones[zoneNumber];
        zoneAliases.delete(zoneNumber);
    }

    function removeTransportFromLayout(zoneNumber, adjacentZoneNumber) {
        const zone = zones[zoneNumber];
        if (zone) {
            zone.transports = zone.transports.filter((t) => t.adjId !== adjacentZoneNumber);
        }
        if (adjacencyList[zoneNumber]) {
            adjacencyList[zoneNumber].delete(adjacentZoneNumber);
            if (adjacencyList[zoneNumber].size === 0) {
                delete adjacencyList[zoneNumber];
            }
        }
        if (adjacencyList[adjacentZoneNumber]) {
            adjacencyList[adjacentZoneNumber].delete(zoneNumber);
            if (adjacencyList[adjacentZoneNumber].size === 0) {
                delete adjacencyList[adjacentZoneNumber];
            }
        }
    }

    function removePassthroughFromLayout(zoneNumber, forwardDestNumber, reverseDestNumber) {
        const zone = zones[zoneNumber];
        if (!zone) {
            return;
        }
        zone.passthroughs = zone.passthroughs.filter((p) => {
            return p.fwd !== forwardDestNumber || p.rev !== reverseDestNumber;
        });
        if (zone.passthroughs.length === 0) {
            zone.height = 140;
        }
    }

    function resetState() {
        nodes.clear();
        links.clear();
        implByAddress.clear();
        objectToImpl.clear();
        interfaceToImplLink.clear();
        stubByObject.clear();
        interfaceProxyIndex.clear();
        interfaceProxyKeyById.clear();
        proxyLinkIndex.clear();
        linkUsage.clear();
        activeZones.clear();
        zoneLayout.clear();
        zoneHierarchy.clear();
        zoneChildren.clear();
        zoneAliases.clear();
        primaryZoneId = null;
        clearObject(zones);
        clearObject(adjacencyList);
        clearObject(PortRegistry);
        transportRefState.clear();
        transportAuditState.completed = false;
        transportAuditState.lastEventTimestamp = 0;
        activeZones.add(primaryZoneId || 1);
        processedIndex = 0;
        processedDisplayTime = 0;
        clearLog();
        ensureZoneNode(0);
        resizeSvg();
        recomputeZoneLayout();
        updateGraph();
    }

    function setCurrentTime(value) {
        currentTime = Math.min(Math.max(value, 0), displayDuration);
        timelineSlider.value = currentTime.toFixed(3);
        const displaySeconds = currentTime;
        const actualSeconds = hasDuration && timeScale > 0 ? currentTime / timeScale : currentTime;
        timeLabel.textContent = `${displaySeconds.toFixed(2)}s`;
        timeLabel.setAttribute('title', `Actual timeline: ${actualSeconds.toFixed(6)}s`);
    }

    function seekTo(targetTime) {
        const clamped = Math.min(Math.max(targetTime, 0), displayDuration);
        if (clamped + epsilon < processedDisplayTime) {
            resetState();
        }
        processUntil(clamped);
        setCurrentTime(clamped);
    }

    function processUntil(targetDisplayTime) {
        while (
            processedIndex < events.length &&
            (eventDisplayTimes[processedIndex] ?? 0) <= targetDisplayTime + epsilon
        ) {
            applyEvent(events[processedIndex]);
            processedDisplayTime = eventDisplayTimes[processedIndex] ?? targetDisplayTime;
            processedIndex += 1;
        }
        updateGraph();
        if (processedIndex >= events.length && !transportAuditState.completed) {
            auditTransportLeaks();
            transportAuditState.completed = true;
        }
    }

    function applyEvent(evt) {
        if (evt && typeof evt.timestamp === 'number') {
            transportAuditState.lastEventTimestamp = evt.timestamp;
        }
        switch (evt.type) {
        case 'service_creation':
            createZone(evt);
            break;
        case 'service_deletion':
            deleteZone(evt);
            break;
        case 'service_proxy_creation':
        case 'cloned_service_proxy_creation':
            createServiceProxy(evt);
            break;
        case 'service_proxy_deletion':
            deleteNode(makeServiceProxyId(evt.data));
            break;
        case 'service_proxy_add_ref':
        case 'service_proxy_release':
        case 'service_proxy_add_external_ref':
        case 'service_proxy_release_external_ref':
            updateProxyCounts(evt);
            break;
        case 'service_try_cast':
        case 'service_add_ref':
            detectZoneHierarchy(evt);
            pulseActivity(evt);
            break;
        case 'service_release':
        case 'service_proxy_try_cast':
            pulseActivity(evt);
            break;
        case 'impl_creation':
            createImpl(evt);
            break;
        case 'impl_deletion':
            deleteImpl(evt);
            break;
        case 'stub_creation':
            createStub(evt);
            break;
        case 'stub_deletion':
            deleteStub(evt);
            break;
        case 'stub_add_ref':
        case 'stub_release':
            updateStubCounts(evt);
            break;
        case 'object_proxy_creation':
            createObjectProxy(evt);
            break;
        case 'object_proxy_deletion':
            deleteNode(makeObjectProxyId(evt.data));
            break;
        case 'interface_proxy_creation':
            createInterfaceProxy(evt);
            break;
        case 'interface_proxy_deletion':
            deleteInterfaceProxy(evt);
            break;
        case 'interface_proxy_send':
        case 'stub_send':
            pulseActivity(evt);
            break;
        case 'transport_creation':
            createTransport(evt);
            break;
        case 'transport_deletion':
            deleteTransport(evt);
            break;
        case 'transport_outbound_add_ref':
        case 'transport_inbound_add_ref':
        case 'transport_outbound_release':
        case 'transport_inbound_release':
            updateTransportRefCounts(evt);
            break;
        case 'pass_through_creation':
            createPassthrough(evt);
            break;
        case 'pass_through_deletion':
            deletePassthrough(evt);
            break;
        case 'pass_through_add_ref':
        case 'pass_through_release':
            updatePassthroughCounts(evt);
            break;
        case 'message':
            appendLog(evt);
            break;
        default:
            appendLog(evt);
            break;
        }
    }

    function createZone(evt) {
        const rawZone = evt.data.zone;
        const zoneNumber = normalizeZoneNumber(rawZone);
        const zoneId = toZoneId(zoneNumber);
        if (!zoneId) {
            appendLog(evt);
            return;
        }
        if (!primaryZoneId && zoneNumber > 0) {
            primaryZoneId = zoneNumber;
        }
        activeZones.add(zoneNumber);
        if (evt.data.serviceName) {
            noteZoneAlias(zoneNumber, evt.data.serviceName);
        }
        const parentNumberRaw = evt.data.parentZone;
        const parentZoneNumber = parentNumberRaw === undefined || parentNumberRaw === null
            ? null
            : normalizeZoneNumber(parentNumberRaw);
        let parentZoneId = toZoneId(parentZoneNumber);
        if (parentZoneNumber === null || parentZoneNumber === zoneNumber) {
            parentZoneId = null;
        } else if (parentZoneId) {
            ensureZoneNode(parentZoneNumber);
        }

        // IMPORTANT: Ensure this zone exists in zones{} for tree layout
        ensureZoneNode(zoneNumber, parentZoneNumber);

        const displayName = formatZoneLabel(zoneNumber);
        const existing = nodes.get(zoneId);
        const refCount = existing?.refCount || 0;
        const node = existing || { id: zoneId, type: 'zone' };
        node.label = displayName;
        node.zone = zoneNumber;
        node.parent = parentZoneId;
        node.refCount = refCount;
        nodes.set(zoneId, node);

        Array.from(links.entries()).forEach(([key, link]) => {
            if (link.type === 'contains' && link.target === zoneId && link.source !== parentZoneId) {
                links.delete(key);
            }
        });

        if (parentZoneId && nodes.has(parentZoneId)) {
            const linkId = `zone-link-${parentZoneId}-${zoneId}`;
            links.set(linkId, {
                id: linkId,
                source: parentZoneId,
                target: zoneId,
                type: 'contains'
            });
        }

        recomputeZoneLayout();
        appendLog(evt);
    }

    function deleteZone(evt) {
        const zoneNumber = normalizeZoneNumber(evt.data.zone);
        const zoneId = `zone-${zoneNumber}`;
        activeZones.delete(zoneNumber);
        const snapshot = Array.from(nodes.values());
        const stack = [zoneId];
        const toRemove = new Set();
        while (stack.length > 0) {
            const current = stack.pop();
            if (toRemove.has(current)) {
                continue;
            }
            toRemove.add(current);
            snapshot.forEach((candidate) => {
                if (!toRemove.has(candidate.id) && candidate.parent === current) {
                    stack.push(candidate.id);
                }
            });
        }
        Array.from(toRemove).forEach((id) => {
            if (id.startsWith('zone-')) {
                const candidate = Number(id.slice(5));
                if (Number.isFinite(candidate)) {
                    removeZoneFromLayout(candidate);
                }
            }
            deleteNode(id);
        });
        appendLog(evt);
    }

    function createServiceProxy(evt) {
        const zoneNumber = normalizeZoneNumber(evt.data.zone);
        const destinationZoneNumber = normalizeZoneNumber(evt.data.destinationZone);
        const callerZoneNumber = evt.data.callerZone === undefined || evt.data.callerZone === null
            ? zoneNumber
            : normalizeZoneNumber(evt.data.callerZone);
        const proxyKeyData = {
            ...evt.data,
            zone: zoneNumber,
            destinationZone: destinationZoneNumber,
            callerZone: callerZoneNumber
        };
        const id = makeServiceProxyId(proxyKeyData);
        const parentZoneId = ensureZoneNode(zoneNumber);
        const destinationZoneId = ensureZoneNode(destinationZoneNumber);
        const label = evt.data.serviceProxyName || evt.data.serviceName || 'service_proxy';
        const node = {
            id: id,
            type: 'service_proxy',
            label: label,
            parent: parentZoneId,
            zone: zoneNumber,
            destinationZone: destinationZoneNumber,
            refCount: 0,
            externalRefCount: 0,
            cloned: evt.type === 'cloned_service_proxy_creation'
        };
        nodes.set(id, node);
        releaseOwnedLinks(id);
        proxyLinkIndex.set(id, new Set());
        if (parentZoneId && nodes.has(parentZoneId)) {
            links.set(`${id}-parent`, {
                id: `${id}-parent`,
                source: parentZoneId,
                target: id,
                type: 'contains'
            });
        }
        if (destinationZoneId && nodes.has(destinationZoneId)) {
            const linkId = `${id}-route-${destinationZoneId}`;
            links.set(linkId, {
                id: linkId,
                source: id,
                target: destinationZoneId,
                type: 'route'
            });
        }
        appendLog(evt);
    }

    function updateProxyCounts(evt) {
        const zoneNumber = normalizeZoneNumber(evt.data.zone);
        const destinationZoneNumber = normalizeZoneNumber(evt.data.destinationZone);
        const callerZoneNumber = evt.data.callerZone === undefined || evt.data.callerZone === null
            ? zoneNumber
            : normalizeZoneNumber(evt.data.callerZone);
        const proxyKeyData = {
            ...evt.data,
            zone: zoneNumber,
            destinationZone: destinationZoneNumber,
            callerZone: callerZoneNumber
        };
        const id = makeServiceProxyId(proxyKeyData);
        if (!nodes.has(id)) {
            return;
        }
        const node = nodes.get(id);
        if (evt.type === 'service_proxy_add_ref') {
            node.refCount = (node.refCount || 0) + 1;
        } else if (evt.type === 'service_proxy_release' && node.refCount > 0) {
            node.refCount -= 1;
        } else if (evt.type === 'service_proxy_add_external_ref') {
            node.externalRefCount = evt.data.refCount;
        } else if (evt.type === 'service_proxy_release_external_ref') {
            node.externalRefCount = evt.data.refCount;
        }
        const channelZoneRaw = evt.data.destinationChannelZone;
        const callerChannelZoneRaw = evt.data.callerChannelZone;
        const channelZoneNumber = channelZoneRaw === undefined || channelZoneRaw === null
            ? null
            : normalizeZoneNumber(channelZoneRaw);
        const callerChannelZoneNumber = callerChannelZoneRaw === undefined || callerChannelZoneRaw === null
            ? null
            : normalizeZoneNumber(callerChannelZoneRaw);
        const parentZoneId = ensureZoneNode(zoneNumber);
        const destinationZoneId = ensureZoneNode(destinationZoneNumber);
        if (channelZoneNumber !== undefined && channelZoneNumber !== null) {
            const channelZoneId = ensureZoneNode(channelZoneNumber);
            if (channelZoneId) {
                const linkId = `${id}-channel-${channelZoneId}`;
                registerOwnedLink(id, linkId, {
                    id: linkId,
                    source: id,
                    target: channelZoneId,
                    type: 'channel_route'
                });
                if (destinationZoneId) {
                    const bridgeId = `channel-bridge-${channelZoneId}-${destinationZoneId}`;
                    registerOwnedLink(id, bridgeId, {
                        id: bridgeId,
                        source: channelZoneId,
                        target: destinationZoneId,
                        type: 'route'
                    });
                }
            }
        }
        if (callerChannelZoneNumber !== undefined && callerChannelZoneNumber !== null && parentZoneId) {
            const callerChannelZoneId = ensureZoneNode(callerChannelZoneNumber);
            if (callerChannelZoneId) {
                const callerBridgeId = `channel-bridge-${parentZoneId}-${callerChannelZoneId}`;
                registerOwnedLink(id, callerBridgeId, {
                    id: callerBridgeId,
                    source: parentZoneId,
                    target: callerChannelZoneId,
                    type: 'route'
                });
            }
        }
        appendLog(evt);
    }

    function createImpl(evt) {
        const zoneNumber = normalizeZoneNumber(evt.data.zone);
        const dataForId = { ...evt.data, zone: zoneNumber };
        const id = makeImplId(dataForId);
        const parentZoneId = ensureZoneNode(zoneNumber);
        const node = {
            id: id,
            type: 'impl',
            label: evt.data.name || `impl@${evt.data.address}`,
            parent: parentZoneId,
            zone: zoneNumber,
            address: evt.data.address,
            count: 1
        };
        nodes.set(id, node);
        if (parentZoneId && nodes.has(parentZoneId)) {
            links.set(`${id}-parent`, {
                id: `${id}-parent`,
                source: parentZoneId,
                target: id,
                type: 'contains'
            });
        }
        const addressKey = evt.data.address !== undefined && evt.data.address !== null ? String(evt.data.address) : null;
        if (addressKey) {
            implByAddress.set(addressKey, id);
            for (const [objectId, stubInfo] of stubByObject.entries()) {
                if (stubInfo.addressKey === addressKey) {
                    objectToImpl.set(objectId, id);
                    refreshInterfaceLinksForObject(objectId);
                }
            }
        }
        appendLog(evt);
    }
)JS";

    constexpr const char* kAnimationScriptPart2 = R"JS(
    function createStub(evt) {
        const zoneNumber = normalizeZoneNumber(evt.data.zone);
        const dataForId = { ...evt.data, zone: zoneNumber };
        const id = makeStubId(dataForId);
        const parentZoneId = ensureZoneNode(zoneNumber);
        const node = {
            id: id,
            type: 'stub',
            label: `stub ${evt.data.object}`,
            parent: parentZoneId,
            zone: zoneNumber,
            object: Number(evt.data.object),
            address: evt.data.address || null,
            refCount: 0
        };
        nodes.set(id, node);
        if (parentZoneId && nodes.has(parentZoneId)) {
            links.set(`${id}-parent`, {
                id: `${id}-parent`,
                source: parentZoneId,
                target: id,
                type: 'contains'
            });
        }
        const objectId = node.object;
        const addressKey = evt.data.address !== undefined && evt.data.address !== null ? String(evt.data.address) : null;
        stubByObject.set(objectId, { id, addressKey });
        if (addressKey) {
            const implId = implByAddress.get(addressKey);
            if (implId) {
                objectToImpl.set(objectId, implId);
                refreshInterfaceLinksForObject(objectId);
            }
        }
        appendLog(evt);
    }

    function updateStubCounts(evt) {
        const id = makeStubId(evt.data);
        if (!nodes.has(id)) {
            return;
        }
        const node = nodes.get(id);
        node.refCount = evt.data.count;
        appendLog(evt);
    }

    function updateTransportRefCounts(evt) {
        const zoneNumber = normalizeZoneNumber(evt.data.zone);
        const adjacentNumber = normalizeZoneNumber(evt.data.adjacentZone);
        const destinationNumber = normalizeZoneNumber(evt.data.destinationZone);
        const callerNumber = normalizeZoneNumber(evt.data.callerZone);
        if (zoneNumber === null || adjacentNumber === null || destinationNumber === null || callerNumber === null) {
            appendLog(evt);
            return;
        }
        const state = ensureTransportState(zoneNumber, adjacentNumber);
        const pairKey = `${destinationNumber}->${callerNumber}`;
        if (!state.pairs.has(pairKey)) {
            state.pairs.set(pairKey, {
                destination: destinationNumber,
                caller: callerNumber,
                inbound: { shared: 0, optimistic: 0 },
                outbound: { shared: 0, optimistic: 0 }
            });
        }
        const entry = state.pairs.get(pairKey);
        const direction = evt.type.includes('outbound') ? 'outbound' : 'inbound';
        const bucket = entry[direction];
        const isAdd = evt.type.includes('add_ref');
        const options = Number(evt.data.options) || 0;
        const optimisticFlag = isAdd
            ? ((options & ADD_REF_OPTIMISTIC) !== 0)
            : ((options & RELEASE_OPTIMISTIC) !== 0);
        const delta = isAdd ? 1 : -1;
        if (optimisticFlag) {
            bucket.optimistic += delta;
            if (bucket.optimistic < 0) {
                const errorTransportId = makeTransportId(zoneNumber, adjacentNumber);
                transportErrors.add(errorTransportId);
                appendTransportAudit(
                    `transport ${errorTransportId} optimistic count went negative`,
                    { transportId: errorTransportId, destination: destinationNumber, caller: callerNumber });
                bucket.optimistic = 0;
            }
        } else {
            bucket.shared += delta;
            if (bucket.shared < 0) {
                const errorTransportId = makeTransportId(zoneNumber, adjacentNumber);
                transportErrors.add(errorTransportId);
                appendTransportAudit(
                    `transport ${errorTransportId} shared count went negative`,
                    { transportId: errorTransportId, destination: destinationNumber, caller: callerNumber });
                bucket.shared = 0;
            }
        }
        if (entry.inbound.shared + entry.outbound.shared === 0
            && entry.inbound.optimistic + entry.outbound.optimistic === 0) {
            state.pairs.delete(pairKey);
        }
        appendLog(evt);
    }

    function createObjectProxy(evt) {
        const zoneNumber = normalizeZoneNumber(evt.data.zone);
        const destinationZoneNumber = normalizeZoneNumber(evt.data.destinationZone);
        const dataForId = { ...evt.data, zone: zoneNumber, destinationZone: destinationZoneNumber };
        const id = makeObjectProxyId(dataForId);
        const parentZoneId = ensureZoneNode(zoneNumber);
        const destinationZoneId = ensureZoneNode(destinationZoneNumber);
        const node = {
            id: id,
            type: 'object_proxy',
            label: `object_proxy ${evt.data.object}`,
            parent: parentZoneId,
            zone: zoneNumber,
            destinationZone: destinationZoneNumber,
            object: evt.data.object,
            addRefDone: !!evt.data.addRefDone
        };
        nodes.set(id, node);
        if (parentZoneId && nodes.has(parentZoneId)) {
            links.set(`${id}-parent`, {
                id: `${id}-parent`,
                source: parentZoneId,
                target: id,
                type: 'contains'
            });
        }
        if (destinationZoneId && nodes.has(destinationZoneId)) {
            const linkId = `${id}-route-${destinationZoneId}`;
            links.set(linkId, {
                id: linkId,
                source: id,
                target: destinationZoneId,
                type: 'route'
            });
        }
        appendLog(evt);
    }

    function createInterfaceProxy(evt) {
        const zoneNumber = normalizeZoneNumber(evt.data.zone);
        const destinationZoneNumber = normalizeZoneNumber(evt.data.destinationZone);
        const proxyData = {
            ...evt.data,
            zone: zoneNumber,
            destinationZone: destinationZoneNumber
        };
        const key = makeInterfaceProxyKey(proxyData);
        let id = makeInterfaceProxyId(proxyData);
        const parentZoneId = ensureZoneNode(zoneNumber);
        const destinationZoneId = ensureZoneNode(destinationZoneNumber);
        const label = evt.data.name || `if ${evt.data.interface}`;
        const objectId = Number(evt.data.object);
        const existingNode = nodes.get(id);
        const node = existingNode || {
            id: id,
            type: 'interface_proxy',
            label: label,
            parent: parentZoneId,
            zone: zoneNumber,
            destinationZone: destinationZoneNumber,
            object: objectId,
            interface: evt.data.interface
        };

        node.label = label;
        node.parent = parentZoneId;
        node.zone = zoneNumber;
        node.destinationZone = destinationZoneNumber;
        node.object = objectId;
        node.interface = evt.data.interface;
        nodes.set(id, node);

        interfaceProxyIndex.set(key, id);
        interfaceProxyKeyById.set(id, key);

        if (parentZoneId && nodes.has(parentZoneId)) {
            links.set(`${id}-parent`, {
                id: `${id}-parent`,
                source: parentZoneId,
                target: id,
                type: 'contains'
            });
        }
        if (destinationZoneId && nodes.has(destinationZoneId)) {
            const linkId = `${id}-route-${destinationZoneId}`;
            links.set(linkId, {
                id: linkId,
                source: id,
                target: destinationZoneId,
                type: 'route'
            });
        }

        refreshInterfaceLinksForObject(node.object);
        appendLog(evt);
    }

    function removeInterfaceLink(interfaceId) {
        if (!interfaceToImplLink.has(interfaceId)) {
            return;
        }
        const linkId = interfaceToImplLink.get(interfaceId);
        interfaceToImplLink.delete(interfaceId);
        links.delete(linkId);
    }

    function registerOwnedLink(ownerId, linkId, linkData) {
        if (ownerId) {
            let owned = proxyLinkIndex.get(ownerId);
            if (!owned) {
                owned = new Set();
                proxyLinkIndex.set(ownerId, owned);
            }
            if (owned.has(linkId)) {
                links.set(linkId, linkData);
                return;
            }
            owned.add(linkId);
        }
        let entry = linkUsage.get(linkId);
        if (!entry) {
            entry = { data: linkData, count: 0 };
            linkUsage.set(linkId, entry);
        }
        entry.count += 1;
        links.set(linkId, linkData);
    }

    function releaseOwnedLinks(ownerId) {
        const owned = proxyLinkIndex.get(ownerId);
        if (!owned) {
            return;
        }
        owned.forEach((linkId) => {
            const entry = linkUsage.get(linkId);
            if (!entry) {
                links.delete(linkId);
                return;
            }
            entry.count -= 1;
            if (entry.count <= 0) {
                linkUsage.delete(linkId);
                links.delete(linkId);
            }
        });
        proxyLinkIndex.delete(ownerId);
    }

    function refreshInterfaceLinksForObject(objectId) {
        if (objectId === undefined || Number.isNaN(objectId)) {
            return;
        }
        const interfaceNodes = Array.from(nodes.values())
            .filter((node) => node.type === 'interface_proxy' && node.object === objectId);
        interfaceNodes.forEach((node) => removeInterfaceLink(node.id));
        const implId = objectToImpl.get(objectId);
        if (!implId || !nodes.has(implId)) {
            return;
        }
        interfaceNodes.forEach((node) => {
            const linkId = `${node.id}-impl-${implId}`;
            links.set(linkId, {
                id: linkId,
                source: node.id,
                target: implId,
                type: 'impl_route'
            });
            interfaceToImplLink.set(node.id, linkId);
        });
    }

    function deleteImpl(evt) {
        const id = makeImplId(evt.data);
        deleteNode(id);
        appendLog(evt);
    }

    function deleteStub(evt) {
        const id = makeStubId(evt.data);
        deleteNode(id);
        appendLog(evt);
    }

    function deleteInterfaceProxy(evt) {
        const key = makeInterfaceProxyKey(evt.data);
        const id = interfaceProxyIndex.get(key) || makeInterfaceProxyId(evt.data);
        if (interfaceProxyIndex.get(key) === id) {
            interfaceProxyIndex.delete(key);
        }
        interfaceProxyKeyById.delete(id);
        deleteNode(id);
        appendLog(evt);
    }

    function makeTransportId(zoneNumber, adjacentZoneNumber) {
        const zone = normalizeZoneNumber(zoneNumber);
        const adjacent = normalizeZoneNumber(adjacentZoneNumber);
        return `transport-${zone}-to-${adjacent}`;
    }

    function makePassthroughId(data) {
        const zone = normalizeZoneNumber(data.zone_id);
        const forward = normalizeZoneNumber(data.forward_destination);
        const reverse = normalizeZoneNumber(data.reverse_destination);
        return `passthrough-${zone}-${forward}-${reverse}`;
    }

    function ensureTransportState(zoneNumber, adjacentZoneNumber) {
        const transportId = makeTransportId(zoneNumber, adjacentZoneNumber);
        if (!transportRefState.has(transportId)) {
            transportRefState.set(transportId, {
                alive: true,
                createdAt: transportAuditState.lastEventTimestamp,
                deletedAt: null,
                pairs: new Map()
            });
        }
        return transportRefState.get(transportId);
    }

    function getTransportTotals(state) {
        let shared = 0;
        let optimistic = 0;
        let nonZeroPairs = 0;
        state.pairs.forEach((entry) => {
            const pairShared = entry.inbound.shared + entry.outbound.shared;
            const pairOptimistic = entry.inbound.optimistic + entry.outbound.optimistic;
            shared += pairShared;
            optimistic += pairOptimistic;
            if (pairShared !== 0 || pairOptimistic !== 0) {
                nonZeroPairs += 1;
            }
        });
        return { shared, optimistic, nonZeroPairs };
    }

    function appendTransportAudit(message, details) {
        appendLog({
            type: 'transport_ref_audit',
            timestamp: transportAuditState.lastEventTimestamp || 0,
            data: { message, ...details }
        });
        if (typeof console !== 'undefined' && console.warn) {
            console.warn('[transport-ref-audit]', message, details || {});
        }
    }

    function auditTransportLeaks() {
        transportRefState.forEach((state, transportId) => {
            if (!state.alive) {
                return;
            }
            const totals = getTransportTotals(state);
            if (totals.shared === 0 && totals.optimistic === 0) {
                appendTransportAudit(
                    `transport ${transportId} still alive with zero ref counts`,
                    { transportId, shared: 0, optimistic: 0 });
            }
        });
    }

    function createTransport(evt) {
        const zone1Number = normalizeZoneNumber(evt.data.zone_id);
        const zone2Number = normalizeZoneNumber(evt.data.adjacent_zone_id);
        const zone1Id = ensureZoneNode(zone1Number);
        const zone2Id = ensureZoneNode(zone2Number);

        const transport1Id = makeTransportId(zone1Number, zone2Number);
        const transport2Id = makeTransportId(zone2Number, zone1Number);

        const transport1 = {
            id: transport1Id,
            type: 'transport',
            label: evt.data.name || 'transport',
            parent: zone1Id,
            zone: zone1Number,
            adjacentZone: zone2Number,
            adjacentZoneId: zone2Id,
            status: evt.data.status
        };
        nodes.set(transport1Id, transport1);
        const state1 = ensureTransportState(zone1Number, zone2Number);
        state1.alive = true;
        state1.createdAt = transportAuditState.lastEventTimestamp;

        const transport2 = {
            id: transport2Id,
            type: 'transport',
            label: evt.data.name || 'transport',
            parent: zone2Id,
            zone: zone2Number,
            adjacentZone: zone1Number,
            adjacentZoneId: zone1Id,
            status: evt.data.status
        };
        nodes.set(transport2Id, transport2);
        const state2 = ensureTransportState(zone2Number, zone1Number);
        state2.alive = true;
        state2.createdAt = transportAuditState.lastEventTimestamp;

        if (zone1Id && nodes.has(zone1Id)) {
            links.set(`${transport1Id}-parent`, {
                id: `${transport1Id}-parent`,
                source: zone1Id,
                target: transport1Id,
                type: 'contains'
            });
        }

        if (zone2Id && nodes.has(zone2Id)) {
            links.set(`${transport2Id}-parent`, {
                id: `${transport2Id}-parent`,
                source: zone2Id,
                target: transport2Id,
                type: 'contains'
            });
        }

        const crossZoneLinkId = `transport-link-${zone1Number}-${zone2Number}`;
        links.set(crossZoneLinkId, {
            id: crossZoneLinkId,
            source: transport1Id,
            target: transport2Id,
            type: 'transport_link'
        });

        // Update zones{} structure for tree layout
        if (zones[zone1Number]) {
            zones[zone1Number].transports.push({ adjId: zone2Number });

            // Update adjacency list
            if (!adjacencyList[zone1Number]) adjacencyList[zone1Number] = new Set();
            if (!adjacencyList[zone2Number]) adjacencyList[zone2Number] = new Set();
            adjacencyList[zone1Number].add(zone2Number);
            adjacencyList[zone2Number].add(zone1Number);
        }

        appendLog(evt);
    }

    function deleteTransport(evt) {
        const zone1Number = normalizeZoneNumber(evt.data.zone_id);
        const zone2Number = normalizeZoneNumber(evt.data.adjacent_zone_id);
        const transport1Id = makeTransportId(zone1Number, zone2Number);
        const transport2Id = makeTransportId(zone2Number, zone1Number);
        const crossZoneLinkId = `transport-link-${zone1Number}-${zone2Number}`;

        links.delete(`${transport1Id}-parent`);
        links.delete(`${transport2Id}-parent`);
        links.delete(crossZoneLinkId);
        deleteNode(transport1Id);
        deleteNode(transport2Id);
        removeTransportFromLayout(zone1Number, zone2Number);
        removeTransportFromLayout(zone2Number, zone1Number);
        [transport1Id, transport2Id].forEach((transportId) => {
            const state = transportRefState.get(transportId);
            if (!state) {
                return;
            }
            const totals = getTransportTotals(state);
            if (totals.shared !== 0 || totals.optimistic !== 0) {
                appendTransportAudit(
                    `transport ${transportId} deleted with nonzero ref counts`,
                    { transportId, shared: totals.shared, optimistic: totals.optimistic, nonZeroPairs: totals.nonZeroPairs });
            }
            state.alive = false;
            state.deletedAt = transportAuditState.lastEventTimestamp;
        });
        appendLog(evt);
    }

    function createPassthrough(evt) {
        const zoneNumber = normalizeZoneNumber(evt.data.zone_id);
        const forwardDestNumber = normalizeZoneNumber(evt.data.forward_destination);
        const reverseDestNumber = normalizeZoneNumber(evt.data.reverse_destination);
        const id = makePassthroughId(evt.data);
        const parentZoneId = ensureZoneNode(zoneNumber);
        ensureZoneNode(forwardDestNumber);
        ensureZoneNode(reverseDestNumber);

        const forwardTransportId = makeTransportId(zoneNumber, forwardDestNumber);
        const reverseTransportId = makeTransportId(zoneNumber, reverseDestNumber);

        const node = {
            id: id,
            type: 'passthrough',
            label: 'passthrough',
            parent: parentZoneId,
            zone: zoneNumber,
            forwardDest: forwardDestNumber,
            reverseDest: reverseDestNumber,
            sharedCount: evt.data.shared_count || 0,
            optimisticCount: evt.data.optimistic_count || 0
        };
        nodes.set(id, node);

        if (parentZoneId && nodes.has(parentZoneId)) {
            links.set(`${id}-parent`, {
                id: `${id}-parent`,
                source: parentZoneId,
                target: id,
                type: 'contains'
            });
        }

        if (nodes.has(forwardTransportId)) {
            links.set(`${id}-forward`, {
                id: `${id}-forward`,
                source: id,
                target: forwardTransportId,
                type: 'passthrough_link'
            });
        }

        if (nodes.has(reverseTransportId)) {
            links.set(`${id}-reverse`, {
                id: `${id}-reverse`,
                source: id,
                target: reverseTransportId,
                type: 'passthrough_link'
            });
        }

        // Update zones{} structure for tree layout
        if (zones[zoneNumber]) {
            zones[zoneNumber].passthroughs.push({
                fwd: forwardDestNumber,
                rev: reverseDestNumber
            });
            // Update height to accommodate passthroughs
            zones[zoneNumber].height = 260;
        }

        appendLog(evt);
    }

    function deletePassthrough(evt) {
        const zoneNumber = normalizeZoneNumber(evt.data.zone_id);
        const forwardDestNumber = normalizeZoneNumber(evt.data.forward_destination);
        const reverseDestNumber = normalizeZoneNumber(evt.data.reverse_destination);
        const id = makePassthroughId(evt.data);
        links.delete(`${id}-forward`);
        links.delete(`${id}-reverse`);
        deleteNode(id);
        removePassthroughFromLayout(zoneNumber, forwardDestNumber, reverseDestNumber);
        appendLog(evt);
    }

    function updatePassthroughCounts(evt) {
        const id = makePassthroughId(evt.data);
        if (!nodes.has(id)) {
            return;
        }
        const node = nodes.get(id);
        if (evt.type === 'pass_through_add_ref') {
            node.sharedCount = (node.sharedCount || 0) + (evt.data.shared_delta || 0);
            node.optimisticCount = (node.optimisticCount || 0) + (evt.data.optimistic_delta || 0);
        } else if (evt.type === 'pass_through_release') {
            node.sharedCount = Math.max(0, (node.sharedCount || 0) + (evt.data.shared_delta || 0));
            node.optimisticCount = Math.max(0, (node.optimisticCount || 0) + (evt.data.optimistic_delta || 0));
        }
        appendLog(evt);
    }

    function detectZoneHierarchy(evt) {
        // Zone hierarchy detection based on service_add_ref events
        // Higher zone numbers are children of lower zone numbers
        const destinationZone = normalizeZoneNumber(evt.data.destinationZone);
        const callerChannelZone = normalizeZoneNumber(evt.data.callerChannelZone);

        if (destinationZone && callerChannelZone && destinationZone !== callerChannelZone) {
            let parentZone, childZone;

            // Determine parent-child relationship: higher number = child, lower number = parent
            if (destinationZone > callerChannelZone) {
                parentZone = callerChannelZone;
                childZone = destinationZone;
            } else {
                parentZone = destinationZone;
                childZone = callerChannelZone;
            }

            // Store the hierarchy relationship
            if (!zoneHierarchy.has(parentZone)) {
                zoneHierarchy.set(parentZone, new Set());
            }
            zoneHierarchy.get(parentZone).add(childZone);

            // Mark child zone for higher level positioning
            zoneChildren.add(childZone);

            // Update zone positioning based on hierarchy
            updateZoneHierarchyPositioning();
        }
    }

    function updateZoneHierarchyPositioning() {
        // Recalculate zone layout considering hierarchy
        zoneLayout.clear();
        const zoneNodes = Array.from(nodes.values()).filter((node) => node.type === 'zone');
        if (zoneNodes.length === 0) {
            return;
        }

        // Create hierarchy-based depth map
        const hierarchyDepths = new Map();

        // Initialize all zones at depth 0
        zoneNodes.forEach((node) => {
            hierarchyDepths.set(node.zone, 0);
        });

        // Apply hierarchy: child zones get higher depth than parents
        zoneHierarchy.forEach((children, parentZone) => {
            const parentDepth = hierarchyDepths.get(parentZone) || 0;
            children.forEach((childZone) => {
                const currentChildDepth = hierarchyDepths.get(childZone) || 0;
                // Child zones should be at least one level higher than parent
                hierarchyDepths.set(childZone, Math.max(currentChildDepth, parentDepth + 1));
            });
        });

        // Sort zones by hierarchy depth and zone number
        const roots = [];
        const childrenByParent = new Map();

        zoneNodes.forEach((node) => {
            // Check if this zone has a hierarchical parent
            let hasHierarchyParent = false;
            for (const [parentZone, children] of zoneHierarchy.entries()) {
                if (children.has(node.zone)) {
                    hasHierarchyParent = true;
                    const parentNode = zoneNodes.find(n => n.zone === parentZone);
                    if (parentNode) {
                        if (!childrenByParent.has(parentNode.id)) {
                            childrenByParent.set(parentNode.id, []);
                        }
                        childrenByParent.get(parentNode.id).push(node);
                    }
                    break;
                }
            }

            if (!hasHierarchyParent) {
                roots.push(node);
            }
        });

        roots.sort((a, b) => (a.zone || 0) - (b.zone || 0));
        let currentColumn = 0;
        const seen = new Set();

        function assign(node, depth) {
            if (!node || seen.has(node.id)) {
                return;
            }
            seen.add(node.id);

            // Use hierarchy depth if available
            const hierarchyDepth = hierarchyDepths.get(node.zone);
            const actualDepth = hierarchyDepth !== undefined ? hierarchyDepth : depth;

            const children = (childrenByParent.get(node.id) || [])
                .slice()
                .sort((a, b) => (a.zone || 0) - (b.zone || 0));

            if (children.length === 0) {
                const order = currentColumn++;
                zoneLayout.set(node.id, { depth: actualDepth, order });
            } else {
                children.forEach((child) => assign(child, actualDepth + 1));
                const childOrders = children
                    .map((child) => zoneLayout.get(child.id)?.order)
                    .filter((value) => value !== undefined);
                const order = childOrders.length > 0
                    ? childOrders.reduce((sum, value) => sum + value, 0) / childOrders.length
                    : currentColumn++;
                zoneLayout.set(node.id, { depth: actualDepth, order });
            }
        }

        roots.forEach((root) => assign(root, hierarchyDepths.get(root.zone) || 0));

        // Finalize positioning
        if (zoneLayout.size === 0) {
            return;
        }

        const maxDepth = Math.max(...Array.from(zoneLayout.values()).map((layout) => layout.depth || 0));
        const baseY = canvasHeight - paddingBottom;
        const minY = zoneBoxHeight / 2 + 80;

        let zoneVerticalSpacing;
        if (maxDepth > 0) {
            const available = Math.max(baseY - minY, zoneBoxHeight);
            const rawSpacing = available / Math.max(maxDepth, 1);
            const minSpacing = Math.max(zoneBoxHeight * 0.6, 140);
            const maxSpacing = zoneBoxHeight + 240;
            let spacing = rawSpacing;
            if (spacing < minSpacing) {
                spacing = Math.min(minSpacing, available / Math.max(maxDepth, 1));
            }
            spacing = Math.min(spacing, maxSpacing);
            if (spacing * maxDepth > available + 1) {
                spacing = rawSpacing;
            }
            zoneVerticalSpacing = Math.max(spacing, zoneBoxHeight * 0.75);
        } else {
            zoneVerticalSpacing = zoneBoxHeight + 160;
        }

        const orders = Array.from(zoneLayout.values()).map((layout) => layout.order);
        const minOrder = Math.min(...orders);
        const orderOffset = minOrder < 0 ? -minOrder : 0;

        zoneLayout.forEach((layout, zoneId) => {
            const adjustedOrder = layout.order + orderOffset;
            const x = 160 + adjustedOrder * zoneColumnSpacing;
            const y = canvasHeight - paddingBottom - layout.depth * zoneVerticalSpacing;
            layout.x = x;
            layout.y = y;
        });

        // Restart simulation to apply new positions
        if (simulation) {
            simulation.alpha(0.3).restart();
        }
    }

    function pulseActivity(evt) {
        const zoneNumber = normalizeZoneNumber(evt.data.zone);
        const targetZoneNumber = normalizeZoneNumber(evt.data.destinationZone || evt.data.destinationChannelZone || evt.data.callerZone);

        // Create a temporary activity link that will be visible for a short time
        if (zoneNumber !== targetZoneNumber && targetZoneNumber) {
            const activityId = `activity-${evt.type}-${evt.timestamp}-${Math.random()}`;
            const activityLink = {
                id: activityId,
                type: 'activity',
                source: `zone-${zoneNumber}`,
                target: `zone-${targetZoneNumber}`,
                label: evt.type.replace(/_/g, ' '),
                timestamp: evt.timestamp,
                temporary: true
            };

            // Add the activity link temporarily
            links.set(activityId, activityLink);

            // Schedule removal of the activity pulse after 2 seconds
            setTimeout(() => {
                links.delete(activityId);
                updateGraph();
            }, 2000);

            updateGraph();
        }

        appendLog(evt);
    }

    function deleteNode(id) {
        if (!nodes.has(id)) {
            return;
        }
        const node = nodes.get(id);
        if (!node) {
            return;
        }
        if (node.type === 'interface_proxy') {
            removeInterfaceLink(id);
            const key = interfaceProxyKeyById.get(id);
            if (key) {
                interfaceProxyKeyById.delete(id);
                if (interfaceProxyIndex.get(key) === id) {
                    interfaceProxyIndex.delete(key);
                }
            }
        } else if (node.type === 'stub') {
            const objectId = Number(node.object);
            stubByObject.delete(objectId);
            objectToImpl.delete(objectId);
            refreshInterfaceLinksForObject(objectId);
        } else if (node.type === 'service_proxy') {
            releaseOwnedLinks(id);
        } else if (node.type === 'impl') {
            if (node.address !== undefined && node.address !== null) {
                implByAddress.delete(String(node.address));
            }
            for (const [objectId, mappedId] of Array.from(objectToImpl.entries())) {
                if (mappedId === id) {
                    objectToImpl.delete(objectId);
                    refreshInterfaceLinksForObject(Number(objectId));
                }
            }
        } else if (node.type === 'zone') {
            zoneLayout.delete(id);
        }
        nodes.delete(id);
        Array.from(links.keys()).forEach((key) => {
            const link = links.get(key);
            if (link.source === id || link.target === id) {
                links.delete(key);
            }
        });
        if (node.type === 'zone') {
            recomputeZoneLayout();
        }
    }

    function makeServiceProxyId(data) {
        const zone = normalizeZoneNumber(data.zone);
        const destination = normalizeZoneNumber(data.destinationZone);
        const callerRaw = data.callerZone;
        const caller = callerRaw === undefined || callerRaw === null
            ? 'self'
            : normalizeZoneNumber(callerRaw);
        return `service-proxy-${zone}-${destination}-${caller}`;
    }

    function makeStubId(data) {
        const zone = normalizeZoneNumber(data.zone);
        return `stub-${zone}-${data.object}`;
    }

    function makeImplId(data) {
        const zone = normalizeZoneNumber(data.zone);
        return `impl-${zone}-${data.address}`;
    }

    function makeObjectProxyId(data) {
        const zone = normalizeZoneNumber(data.zone);
        const destination = normalizeZoneNumber(data.destinationZone);
        return `object-proxy-${zone}-${destination}-${data.object}`;
    }

    function makeInterfaceProxyKey(data) {
        const zone = normalizeZoneNumber(data.zone);
        const destination = normalizeZoneNumber(data.destinationZone);
        return `${zone}-${destination}-${data.object}`;
    }

    function makeInterfaceProxyId(data) {
        const key = makeInterfaceProxyKey(data);
        const existing = interfaceProxyIndex.get(key);
        if (existing) {
            return existing;
        }
        return `interface-proxy-${key}`;
    }

    function findClosestZone(x, y) {
        const candidates = Array.from(nodes.values()).filter((node) => node.type === 'zone');
        if (candidates.length === 0) {
            return null;
        }
        let closest = null;
        let distance = Infinity;
        for (const node of candidates) {
            const dx = (node.x ?? 0) - x;
            const dy = (node.y ?? 0) - y;
            const dist = Math.sqrt(dx * dx + dy * dy);
            if (dist < distance && dist < 120) {
                distance = dist;
                closest = node;
            }
        }
        return closest;
    }

    function makeTooltip(node) {
        const metadata = zoneMetadata[node.zone] || {};
        const parentZoneRaw = metadata.parent !== undefined ? normalizeZoneNumber(metadata.parent) : null;
        const parentDesc = parentZoneRaw && parentZoneRaw !== node.zone
            ? `parent zone ${parentZoneRaw}`
            : 'top-level zone';
        return `<strong>${formatZoneLabel(node.zone)}</strong><br/>${parentDesc}`;
    }

    function appendLog(evt) {
        const li = document.createElement('li');
        const time = evt.timestamp.toFixed(3).padStart(7, ' ');
        li.innerHTML = `<div class="timestamp">${time}s · ${evt.type}</div><div>${formatEventSummary(evt)}</div>`;
        eventLog.appendChild(li);
        while (eventLog.childNodes.length > maxLogEntries) {
            eventLog.removeChild(eventLog.firstChild);
        }
        if (eventLogBody && !logCollapsed) {
            eventLogBody.scrollTop = eventLogBody.scrollHeight;
        }
    }

    function clearLog() {
        eventLog.textContent = '';
    }

    function formatEventSummary(evt) {
        const data = evt.data;
        switch (evt.type) {
        case 'transport_ref_audit':
            return data.message || 'transport ref audit';
        case 'service_creation':
            return `zone ${data.zone} created (parent ${data.parentZone})`;
        case 'service_deletion':
            return `zone ${data.zone} deleted`;
        case 'service_proxy_creation':
        case 'cloned_service_proxy_creation':
            return `service proxy ${data.serviceProxyName || ''} from zone ${data.zone} to ${data.destinationZone}`;
        case 'service_proxy_deletion':
            return `service proxy removed in zone ${data.zone}`;
        case 'impl_creation':
            return `impl ${data.name} created @${data.address} in zone ${data.zone}`;
        case 'impl_deletion':
            return `impl ${data.address} removed from zone ${data.zone}`;
        case 'stub_creation':
            return `stub ${data.object} created in zone ${data.zone}`;
        case 'stub_deletion':
            return `stub ${data.object} removed from zone ${data.zone}`;
        case 'object_proxy_creation':
            return `object proxy ${data.object} from zone ${data.zone} to ${data.destinationZone}`;
        case 'interface_proxy_creation':
            return `interface proxy ${data.name} on object ${data.object}`;
        case 'pass_through_creation':
            return `passthrough in zone ${data.zone_id}: fwd→${data.forward_destination} rev→${data.reverse_destination}`;
        case 'pass_through_deletion':
            return `passthrough deleted in zone ${data.zone_id}: fwd→${data.forward_destination} rev→${data.reverse_destination}`;
        case 'pass_through_add_ref':
            return `passthrough add_ref in zone ${data.zone_id}: S+${data.shared_delta || 0} O+${data.optimistic_delta || 0}`;
        case 'pass_through_release':
            return `passthrough release in zone ${data.zone_id}: S${data.shared_delta || 0} O${data.optimistic_delta || 0}`;
        case 'message':
            return data.message || 'telemetry message';
        default:
            return Object.keys(data || {}).map((key) => `${key}: ${data[key]}`).join(', ');
        }
    }

    function renderLegend() {
        const legend = d3.select('body').append('div').attr('class', 'legend');
        const info = legend.append('div').attr('class', 'info-banner');
        info.html('Zoom with mouse wheel · Pan by dragging the canvas · Drag nodes to pin positions');
        const items = legend.append('div');
        const groups = [
            { key: 'zone', label: 'Zone (Service)' },
            { key: 'transport', label: 'Transport' },
            { key: 'service_proxy', label: 'Service Proxy' },
            { key: 'passthrough', label: 'Passthrough' },
            { key: 'object_proxy', label: 'Object Proxy' },
            { key: 'interface_proxy', label: 'Interface Proxy' },
            { key: 'impl', label: 'Implementation Object' },
            { key: 'stub', label: 'Stub' }
        ];
        items.selectAll('span.item')
            .data(groups)
            .enter()
            .append('span')
            .attr('class', 'item')
            .html((d) => {
                const extraClass = d.key === 'zone' ? ' zone' : '';
                return `<span class="swatch${extraClass}" style="background:${palette[d.key]}"></span>${d.label}`;
            });
    }

    function getTransportPairLines(zoneNumber, adjacentNumber) {
        const transportId = makeTransportId(zoneNumber, adjacentNumber);
        const state = transportRefState.get(transportId);
        if (!state || state.pairs.size === 0) {
            return [];
        }
        const entries = Array.from(state.pairs.values())
            .map((entry) => {
                const shared = entry.inbound.shared + entry.outbound.shared;
                const optimistic = entry.inbound.optimistic + entry.outbound.optimistic;
                return {
                    destination: entry.destination,
                    caller: entry.caller,
                    shared,
                    optimistic
                };
            })
            .sort((a, b) => (a.destination - b.destination) || (a.caller - b.caller));
        return entries.map((entry) =>
            `D${entry.destination} C${entry.caller} S${entry.shared} O${entry.optimistic}`);
    }

    function buildTransportLines(zoneNumber, adjacentNumber, header) {
        const detailLines = getTransportPairLines(zoneNumber, adjacentNumber);
        const lines = [header];
        if (detailLines.length === 0) {
            lines.push('no refs');
        } else {
            lines.push(...detailLines);
        }
        return lines;
    }

    function computeTransportMetrics(zoneNumber, adjacentNumber, header) {
        const lines = buildTransportLines(zoneNumber, adjacentNumber, header);
        const maxLen = lines.reduce((max, line) => Math.max(max, line.length), 0);
        const width = Math.min(
            transportMaxWidth,
            Math.max(transportMinWidth, transportBoxPaddingX * 2 + maxLen * 6));
        const height = Math.max(
            transportMinHeight,
            transportBoxPaddingY * 2 + lines.length * transportLineHeight);
        return { lines, width, height };
    }

    function buildPassthroughLines(forwardDest, reverseDest, sharedCount, optimisticCount) {
        const lines = [
            `FWD:${forwardDest} REV:${reverseDest}`,
            `S${sharedCount || 0} O${optimisticCount || 0}`
        ];
        return lines;
    }

    function computePassthroughMetrics(forwardDest, reverseDest, sharedCount, optimisticCount) {
        const lines = buildPassthroughLines(forwardDest, reverseDest, sharedCount, optimisticCount);
        const maxLen = lines.reduce((max, line) => Math.max(max, line.length), 0);
        const width = Math.min(
            passthroughMaxWidth,
            Math.max(passthroughMinWidth, passthroughBoxPaddingX * 2 + maxLen * 6));
        const height = Math.max(
            passthroughMinHeight,
            passthroughBoxPaddingY * 2 + lines.length * passthroughLineHeight);
        return { lines, width, height };
    }

    function rebuildVisualization() {
        // Clear existing visualization
        g.selectAll('*').remove();
        clearObject(PortRegistry);

        // Find all root zones (zones with parentId === 0)
        const rootZones = Object.values(zones).filter(z => z.parentId === 0);
        if (rootZones.length === 0) {
            return;
        }

        // Create virtual root zone (zone 0) to contain all root zones
        const virtualRoot = {
            id: 0,
            name: 'Virtual Root',
            parentId: -1,
            transports: [],
            passthroughs: [],
            children: rootZones.map(z => z.id),
            width: 260,
            height: 140
        };

        // Build D3 hierarchy from zones{}
        const buildHierarchy = (z) => {
            return {
                id: z.id,
                data: z,
                children: z.children
                    .filter(cid => zones[cid] !== undefined)
                    .map(cid => buildHierarchy(zones[cid]))
            };
        };

        const root = d3.hierarchy(buildHierarchy(virtualRoot));

        // Calculate zone dimensions
        Object.values(zones).forEach(z => {
            z.transportMetrics = {};
            const adjacentIds = new Set(z.transports.map((t) => t.adjId));
            if (z.parentId && z.parentId !== 0) {
                adjacentIds.add(z.parentId);
            }
            let maxTransportBoxWidth = transportMinWidth;
            let maxTransportBoxHeight = transportMinHeight;
            adjacentIds.forEach((adjId) => {
                const header = (z.parentId && adjId === z.parentId) ? `IN:${adjId}` : `TO:${adjId}`;
                const metrics = computeTransportMetrics(z.id, adjId, header);
                z.transportMetrics[adjId] = metrics;
                maxTransportBoxWidth = Math.max(maxTransportBoxWidth, metrics.width);
                maxTransportBoxHeight = Math.max(maxTransportBoxHeight, metrics.height);
            });
            z.transportBoxWidth = maxTransportBoxWidth;
            z.transportBoxHeight = maxTransportBoxHeight;

            // Calculate passthrough metrics
            z.passthroughMetrics = [];
            let maxPassthroughBoxWidth = passthroughMinWidth;
            let maxPassthroughBoxHeight = passthroughMinHeight;
            z.passthroughs.forEach((p) => {
                const passthroughId = makePassthroughId({ zone_id: z.id, forward_destination: p.fwd, reverse_destination: p.rev });
                const passthroughNode = nodes.get(passthroughId);
                const sharedCount = passthroughNode ? passthroughNode.sharedCount : 0;
                const optimisticCount = passthroughNode ? passthroughNode.optimisticCount : 0;
                const metrics = computePassthroughMetrics(p.fwd, p.rev, sharedCount, optimisticCount);
                z.passthroughMetrics.push(metrics);
                maxPassthroughBoxWidth = Math.max(maxPassthroughBoxWidth, metrics.width);
                maxPassthroughBoxHeight = Math.max(maxPassthroughBoxHeight, metrics.height);
            });
            z.passthroughBoxWidth = maxPassthroughBoxWidth;
            z.passthroughBoxHeight = maxPassthroughBoxHeight;

            const colCount = Math.max(z.transports.length, z.passthroughs.length, 1);
            const maxBoxWidth = Math.max(maxTransportBoxWidth, maxPassthroughBoxWidth);
            z.width = Math.max(260, colCount * (maxBoxWidth + 40));
            const baseHeight = z.passthroughs.length > 0 ? 260 : 140;
            const totalHeight = 120 + maxTransportBoxHeight + (z.passthroughs.length > 0 ? maxPassthroughBoxHeight : 0);
            z.height = Math.max(baseHeight, totalHeight);
        });

        // Apply tree layout
        const maxZWidth = d3.max(Object.values(zones), z => z.width) || 260;
        d3.tree().nodeSize([maxZWidth + 150, 600])(root);

        // Calculate bounding box of all nodes in tree coordinate space
        const descendants = root.descendants();
        const xExtent = d3.extent(descendants, d => d.x);
        const yExtent = d3.extent(descendants, d => d.y);
        const maxZoneHeight = d3.max(Object.values(zones), z => z.height) || 260;

        // Calculate viewBox dimensions with padding for zone sizes
        const padding = 200;
        const viewBoxX = xExtent[0] - maxZWidth / 2 - padding;
        const viewBoxY = yExtent[0] - maxZoneHeight - padding;
        const viewBoxWidth = (xExtent[1] - xExtent[0]) + maxZWidth + 2 * padding;
        const viewBoxHeight = (yExtent[1] - yExtent[0]) + maxZoneHeight * 2 + 2 * padding;

        // Set viewBox to encompass entire tree in logical coordinates
        svgRoot
            .attr('width', canvasWidth)
            .attr('height', canvasHeight)
            .attr('viewBox', `${viewBoxX} ${-viewBoxY - viewBoxHeight} ${viewBoxWidth} ${viewBoxHeight}`)
            .attr('preserveAspectRatio', 'xMidYMid meet');

        // Use tree coordinates directly (no transformation needed)
        // Root at y=0 (bottom), children grow upward (positive y)
        const getX = d => d.x;
        const getY = d => -d.y;  // Flip y-axis so root is at bottom

        // Build Port Registry with absolute coordinates (skip virtual root zone 0)
        root.descendants().filter(d => d.data.id !== 0).forEach(d => {
            const z = d.data.data;
            const absX = getX(d);
            const absY = getY(d);

            // IN port (parent connection at top center)
            if (d.parent && d.parent.data.id !== 0) {
                const parentMetrics = z.transportMetrics
                    ? z.transportMetrics[d.parent.data.id]
                    : null;
                PortRegistry[`${z.id}:${d.parent.data.id}`] = {
                    relX: 0, relY: 0,
                    absX: absX, absY: absY,
                    boxWidth: parentMetrics ? parentMetrics.width : transportMinWidth,
                    boxHeight: parentMetrics ? parentMetrics.height : transportMinHeight,
                    lines: parentMetrics ? parentMetrics.lines : [`IN:${d.parent.data.id}`, 'no refs']
                };
            }

            // OUT ports (child connections at bottom, distributed evenly)
            z.transports.forEach((t, i) => {
                const transportMetrics = z.transportMetrics
                    ? z.transportMetrics[t.adjId]
                    : null;
                const tx = (z.transports.length > 1)
                    ? (i / (z.transports.length - 1) * (z.width - 140)) - (z.width / 2 - 70)
                    : 0;
                const ty = -z.height;
                PortRegistry[`${z.id}:${t.adjId}`] = {
                    relX: tx, relY: ty,
                    absX: absX + tx, absY: absY + ty,
                    boxWidth: transportMetrics ? transportMetrics.width : transportMinWidth,
                    boxHeight: transportMetrics ? transportMetrics.height : transportMinHeight,
                    lines: transportMetrics ? transportMetrics.lines : [`TO:${t.adjId}`, 'no refs']
                };
            });
        });

        // Draw trunk lines (behind zones) - skip connections involving virtual root zone 0
        g.selectAll('.trunk-line')
            .data(root.descendants().filter(d => d.parent && d.data.id !== 0 && d.parent.data.id !== 0))
            .enter().append('line')
            .attr('class', 'trunk-line')
            .attr('x1', d => PortRegistry[`${d.parent.data.id}:${d.data.id}`].absX)
            .attr('y1', d => PortRegistry[`${d.parent.data.id}:${d.data.id}`].absY)
            .attr('x2', d => PortRegistry[`${d.data.id}:${d.parent.data.id}`].absX)
            .attr('y2', d => PortRegistry[`${d.data.id}:${d.parent.data.id}`].absY);

        // Draw zones with internal circuitry (skip virtual root zone 0)
        const nodeGroups = g.selectAll('.node')
            .data(root.descendants().filter(d => d.data.id !== 0))
            .enter().append('g')
            .attr('transform', d => `translate(${getX(d)},${getY(d)})`);

        nodeGroups.each(function(d) {
            const zoneSel = d3.select(this);
            const z = d.data.data;
            const svcY = -z.height / 2;

            // Zone background
            zoneSel.append('rect')
                .attr('class', 'zone-bg')
                .attr('x', -z.width / 2)
                .attr('y', -z.height)
                .attr('width', z.width)
                .attr('height', z.height)
                .attr('rx', 10);

            // Service box (center)
            zoneSel.append('rect')
                .attr('class', 'service-box')
                .attr('x', -30)
                .attr('y', svcY - 15)
                .attr('width', 60)
                .attr('height', 30);

            zoneSel.append('text')
                .attr('class', 'label')
                .attr('y', svcY + 5)
                .text(z.name.toUpperCase());

            // Render ports and wires
            Object.keys(PortRegistry).forEach(key => {
                const [zId, adjId] = key.split(':').map(Number);
                if (zId !== z.id) return;

                const p = PortRegistry[key];
                const pG = zoneSel.append('g').attr('transform', `translate(${p.relX},${p.relY})`);

                const boxWidth = p.boxWidth || transportMinWidth;
                const boxHeight = p.boxHeight || transportMinHeight;
                const lines = p.lines || [p.relY === 0 ? `IN:${adjId}` : `TO:${adjId}`];

                // Check if this transport has errors
                const transportId = makeTransportId(zId, adjId);
                const hasError = transportErrors.has(transportId);

                pG.append('rect')
                    .attr('class', 'transport-box')
                    .attr('x', -boxWidth / 2)
                    .attr('y', -boxHeight / 2)
                    .attr('width', boxWidth)
                    .attr('height', boxHeight)
                    .attr('rx', 4)
                    .attr('stroke', hasError ? '#ff0000' : null)
                    .attr('stroke-width', hasError ? 3 : null);

                const textStartX = -boxWidth / 2 + transportBoxPaddingX;
                const textStartY = -boxHeight / 2 + transportBoxPaddingY + 9;
                lines.forEach((line, idx) => {
                    pG.append('text')
                        .attr('class', idx === 0 ? 'transport-label' : 'transport-detail')
                        .attr('x', textStartX)
                        .attr('y', textStartY + idx * transportLineHeight)
                        .attr('text-anchor', 'start')
                        .text(line);
                });

                // Wire from service to port
                const halfBoxHeight = boxHeight / 2;
                zoneSel.append('line')
                    .attr('class', 'wire')
                    .attr('x1', p.relX)
                    .attr('y1', p.relY + (p.relY === 0 ? -halfBoxHeight : halfBoxHeight))
                    .attr('x2', 0)
                    .attr('y2', svcY + (p.relY === 0 ? 15 : -15));
            });

            // Render passthroughs
            z.passthroughs.forEach((p, i) => {
                const px = (z.passthroughs.length > 1)
                    ? (i / (z.passthroughs.length - 1) * (z.width - 160)) - (z.width / 2 - 80)
                    : 0;
                const py = svcY + 80;

                const metrics = z.passthroughMetrics[i] || computePassthroughMetrics(p.fwd, p.rev, 0, 0);
                const boxWidth = metrics.width;
                const boxHeight = metrics.height;
                const lines = metrics.lines;

                const pG = zoneSel.append('g').attr('transform', `translate(${px},${py})`);

                pG.append('rect')
                    .attr('class', 'pass-box')
                    .attr('x', -boxWidth / 2)
                    .attr('y', -boxHeight / 2)
                    .attr('width', boxWidth)
                    .attr('height', boxHeight)
                    .attr('rx', 4);

                const textStartX = -boxWidth / 2 + passthroughBoxPaddingX;
                const textStartY = -boxHeight / 2 + passthroughBoxPaddingY + 9;
                lines.forEach((line, idx) => {
                    pG.append('text')
                        .attr('class', idx === 0 ? 'pass-label' : 'pass-detail')
                        .attr('x', textStartX)
                        .attr('y', textStartY + idx * passthroughLineHeight)
                        .attr('text-anchor', 'start')
                        .text(line);
                });

                // Route wires through passthrough
                const nextHopRev = findNextHop(z.id, p.rev);
                const nextHopFwd = findNextHop(z.id, p.fwd);
                const rP = PortRegistry[`${z.id}:${nextHopRev}`];
                const fP = PortRegistry[`${z.id}:${nextHopFwd}`];

                if (rP && fP && (rP !== fP)) {
                    const halfBoxHeight = boxHeight / 2;
                    // Wire from reverse port to passthrough
                    zoneSel.append('line')
                        .attr('class', 'wire routing')
                        .attr('x1', rP.relX)
                        .attr('y1', rP.relY + (rP.relY === 0 ? -15 : 15))
                        .attr('x2', px)
                        .attr('y2', py + halfBoxHeight);

                    // Wire from passthrough to forward port
                    zoneSel.append('line')
                        .attr('class', 'wire routing')
                        .attr('x1', px)
                        .attr('y1', py - halfBoxHeight)
                        .attr('x2', fP.relX)
                        .attr('y2', fP.relY + (fP.relY === 0 ? -15 : 15));
                }
                // If routing fails, passthrough is rendered without wiring
            });
        });
    }

    function updateGraph() {
        // Legacy function redirects to new tree layout implementation
        rebuildVisualization();
    }

    function truncateLabel(label) {
        if (!label) {
            return '';
        }
        return label.length > 18 ? `${label.slice(0, 15)}…` : label;
    }
})();
)JS";
}

namespace rpc
{
    namespace
    {
        constexpr const char* kTitlePrefix = "RPC++ Telemetry Animation";
    }

    bool animation_telemetry_service::create(std::shared_ptr<rpc::i_telemetry_service>& service,
        const std::string& test_suite_name,
        const std::string& name,
        const std::filesystem::path& directory)
    {
        auto fixed_suite = sanitize_name(test_suite_name);
        std::filesystem::path output_directory = directory / fixed_suite;
        std::error_code ec;
        std::filesystem::create_directories(output_directory, ec);

        auto output_path = output_directory / (name + ".html");

        service = std::shared_ptr<rpc::i_telemetry_service>(
            new animation_telemetry_service(output_path, test_suite_name, name));
        return true;
    }

    animation_telemetry_service::animation_telemetry_service(
        std::filesystem::path output_path, std::string test_suite_name, std::string test_name)
        : output_path_(std::move(output_path))
        , suite_name_(std::move(test_suite_name))
        , test_name_(std::move(test_name))
        , start_time_(std::chrono::steady_clock::now())
    {
    }

    animation_telemetry_service::~animation_telemetry_service()
    {
        write_output();
    }

    std::string animation_telemetry_service::sanitize_name(const std::string& name)
    {
        std::string sanitized = name;
        std::replace_if(
            sanitized.begin(), sanitized.end(), [](char ch) { return ch == '/' || ch == '\\' || ch == ':' || ch == '*'; }, '#');
        return sanitized;
    }

    std::string animation_telemetry_service::escape_json(const std::string& input)
    {
        std::string escaped;
        escaped.reserve(input.size() + input.size() / 4 + 4);
        for (char ch : input)
        {
            switch (ch)
            {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20)
                {
                    std::ostringstream oss;
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(ch));
                    escaped += oss.str();
                }
                else
                {
                    escaped += ch;
                }
            }
        }
        return escaped;
    }

    animation_telemetry_service::event_field animation_telemetry_service::make_string_field(
        const std::string& key, const std::string& value)
    {
        return event_field{key, value, field_kind::string};
    }

    animation_telemetry_service::event_field animation_telemetry_service::make_number_field(
        const std::string& key, uint64_t value)
    {
        return event_field{key, std::to_string(value), field_kind::number};
    }

    animation_telemetry_service::event_field animation_telemetry_service::make_signed_field(
        const std::string& key, int64_t value)
    {
        return event_field{key, std::to_string(value), field_kind::number};
    }

    animation_telemetry_service::event_field animation_telemetry_service::make_boolean_field(
        const std::string& key, bool value)
    {
        return event_field{key, value ? "true" : "false", field_kind::boolean};
    }

    animation_telemetry_service::event_field animation_telemetry_service::make_floating_field(
        const std::string& key, double value)
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6) << value;
        return event_field{key, oss.str(), field_kind::floating};
    }

    void animation_telemetry_service::record_event(const std::string& type, std::initializer_list<event_field> fields) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        event_record record;
        record.timestamp = timestamp_now();
        record.type = type;
        record.fields.assign(fields.begin(), fields.end());
        events_.push_back(std::move(record));
    }

    void animation_telemetry_service::record_event(const std::string& type, std::vector<event_field>&& fields) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        event_record record;
        record.timestamp = timestamp_now();
        record.type = type;
        record.fields = std::move(fields);
        events_.push_back(std::move(record));
    }

    void animation_telemetry_service::on_service_creation(
        const std::string& name, rpc::zone zone_id, rpc::destination_zone parent_zone_id) const
    {
        double ts = timestamp_now();
        std::vector<event_field> fields;
        fields.reserve(3);
        fields.push_back(make_string_field("serviceName", name));
        fields.push_back(make_number_field("zone", zone_id.get_val()));
        fields.push_back(make_number_field("parentZone", parent_zone_id.get_val()));

        {
            std::lock_guard<std::mutex> lock(mutex_);
            zone_names_[zone_id.get_val()] = name;
            zone_parents_[zone_id.get_val()] = parent_zone_id.get_val();

            event_record record;
            record.timestamp = ts;
            record.type = "service_creation";
            record.fields = std::move(fields);
            events_.push_back(std::move(record));
        }
    }

    void animation_telemetry_service::on_service_deletion(rpc::zone zone_id) const
    {
        std::vector<event_field> fields = {make_number_field("zone", zone_id.get_val())};
        std::lock_guard<std::mutex> lock(mutex_);
        zone_names_.erase(zone_id.get_val());
        zone_parents_.erase(zone_id.get_val());

        event_record record;
        record.timestamp = timestamp_now();
        record.type = "service_deletion";
        record.fields = std::move(fields);
        events_.push_back(std::move(record));
    }

    void animation_telemetry_service::on_service_try_cast(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        record_event("service_try_cast",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val())});
    }

    void animation_telemetry_service::on_service_add_ref(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options options,
        uint64_t reference_count) const
    {
        record_event("service_add_ref",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("knownDirectionZone", known_direction_zone_id.get_val()),
                make_number_field("options", static_cast<uint64_t>(options)),
                make_number_field("referenceCount", reference_count)});
    }

    void animation_telemetry_service::on_service_release(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::release_options options,
        uint64_t reference_count) const
    {
        record_event("service_release",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("options", static_cast<uint64_t>(options)),
                make_number_field("referenceCount", reference_count)});
    }

    void animation_telemetry_service::on_service_proxy_creation(const std::string& service_name,
        const std::string& service_proxy_name,
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        std::vector<event_field> fields = {make_string_field("serviceName", service_name),
            make_string_field("serviceProxyName", service_proxy_name),
            make_number_field("zone", zone_id.get_val()),
            make_number_field("destinationZone", destination_zone_id.get_val()),
            make_number_field("callerZone", caller_zone_id.get_val())};
        record_event("service_proxy_creation", std::move(fields));
    }

    void animation_telemetry_service::on_cloned_service_proxy_creation(const std::string& service_name,
        const std::string& service_proxy_name,
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        std::vector<event_field> fields = {make_string_field("serviceName", service_name),
            make_string_field("serviceProxyName", service_proxy_name),
            make_number_field("zone", zone_id.get_val()),
            make_number_field("destinationZone", destination_zone_id.get_val()),
            make_number_field("callerZone", caller_zone_id.get_val())};
        record_event("cloned_service_proxy_creation", std::move(fields));
    }

    void animation_telemetry_service::on_service_proxy_deletion(
        rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
    {
        record_event("service_proxy_deletion",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val())});
    }

    void animation_telemetry_service::on_service_proxy_try_cast(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        record_event("service_proxy_try_cast",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val())});
    }

    void animation_telemetry_service::on_service_proxy_add_ref(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options options,
        uint64_t reference_count) const
    {
        record_event("service_proxy_add_ref",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("knownDirectionZone", known_direction_zone_id.get_val()),
                make_number_field("options", static_cast<uint64_t>(options)),
                make_number_field("referenceCount", reference_count)});
    }

    void animation_telemetry_service::on_service_proxy_release(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::release_options options,
        uint64_t reference_count) const
    {
        record_event("service_proxy_release",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("options", static_cast<uint64_t>(options)),
                make_number_field("referenceCount", reference_count)});
    }

    void animation_telemetry_service::on_service_proxy_add_external_ref(
        rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id, int ref_count) const
    {
        record_event("service_proxy_add_external_ref",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_signed_field("refCount", ref_count)});
    }

    void animation_telemetry_service::on_service_proxy_release_external_ref(
        rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id, int ref_count) const
    {
        record_event("service_proxy_release_external_ref",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_signed_field("refCount", ref_count)});
    }

    void animation_telemetry_service::on_impl_creation(const std::string& name, uint64_t address, rpc::zone zone_id) const
    {
        std::vector<event_field> fields = {make_string_field("name", name),
            make_number_field("address", address),
            make_number_field("zone", zone_id.get_val())};
        record_event("impl_creation", std::move(fields));
    }

    void animation_telemetry_service::on_impl_deletion(uint64_t address, rpc::zone zone_id) const
    {
        record_event(
            "impl_deletion", {make_number_field("address", address), make_number_field("zone", zone_id.get_val())});
    }

    void animation_telemetry_service::on_stub_creation(rpc::zone zone_id, rpc::object object_id, uint64_t address) const
    {
        record_event("stub_creation",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("address", address)});
    }

    void animation_telemetry_service::on_stub_deletion(rpc::zone zone_id, rpc::object object_id) const
    {
        record_event("stub_deletion",
            {make_number_field("zone", zone_id.get_val()), make_number_field("object", object_id.get_val())});
    }

    void animation_telemetry_service::on_stub_send(
        rpc::zone zone_id, rpc::object object_id, rpc::interface_ordinal interface_id, rpc::method method_id) const
    {
        record_event("stub_send",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
    }

    void animation_telemetry_service::on_stub_add_ref(rpc::zone zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        uint64_t count,
        rpc::caller_zone caller_zone_id) const
    {
        record_event("stub_add_ref",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("count", count),
                make_number_field("callerZone", caller_zone_id.get_val())});
    }

    void animation_telemetry_service::on_stub_release(rpc::zone zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        uint64_t count,
        rpc::caller_zone caller_zone_id) const
    {
        record_event("stub_release",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("count", count),
                make_number_field("callerZone", caller_zone_id.get_val())});
    }

    void animation_telemetry_service::on_object_proxy_creation(
        rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::object object_id, bool add_ref_done) const
    {
        record_event("object_proxy_creation",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_boolean_field("addRefDone", add_ref_done)});
    }

    void animation_telemetry_service::on_object_proxy_deletion(
        rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::object object_id) const
    {
        record_event("object_proxy_deletion",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("object", object_id.get_val())});
    }

    void animation_telemetry_service::on_interface_proxy_creation(const std::string& name,
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        std::vector<event_field> fields = {make_string_field("name", name),
            make_number_field("zone", zone_id.get_val()),
            make_number_field("destinationZone", destination_zone_id.get_val()),
            make_number_field("object", object_id.get_val()),
            make_number_field("interface", interface_id.get_val())};
        record_event("interface_proxy_creation", std::move(fields));
    }

    void animation_telemetry_service::on_interface_proxy_deletion(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        record_event("interface_proxy_deletion",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val())});
    }

    void animation_telemetry_service::on_interface_proxy_send(const std::string& method_name,
        rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        std::vector<event_field> fields = {make_string_field("methodName", method_name),
            make_number_field("zone", zone_id.get_val()),
            make_number_field("destinationZone", destination_zone_id.get_val()),
            make_number_field("object", object_id.get_val()),
            make_number_field("interface", interface_id.get_val()),
            make_number_field("method", method_id.get_val())};
        record_event("interface_proxy_send", std::move(fields));
    }

    void animation_telemetry_service::message(level_enum level, const std::string& message) const
    {
        std::vector<event_field> fields
            = {make_number_field("level", static_cast<uint64_t>(level)), make_string_field("message", message)};
        record_event("message", std::move(fields));
    }

    void animation_telemetry_service::write_output() const
    {
        std::vector<event_record> events_copy;
        std::unordered_map<uint64_t, std::string> zone_names_copy;
        std::unordered_map<uint64_t, uint64_t> zone_parents_copy;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            events_copy = events_;
            zone_names_copy = zone_names_;
            zone_parents_copy = zone_parents_;
        }

        std::error_code ec;
        std::filesystem::create_directories(output_path_.parent_path(), ec);

        std::ofstream output(output_path_, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!output)
        {
            return;
        }

        double total_duration = 0.0;
        if (!events_copy.empty())
        {
            total_duration = events_copy.back().timestamp;
            for (const auto& evt : events_copy)
            {
                if (evt.timestamp > total_duration)
                {
                    total_duration = evt.timestamp;
                }
            }
        }

        output << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\" />\n";
        output << "<title>" << kTitlePrefix << " - " << escape_json(suite_name_) << "." << escape_json(test_name_)
               << "</title>\n";
        output << "<style>\n" << kAnimationStyles << "\n</style>\n";
        output << "</head>\n<body>\n";
        output << "<div class=\"header\">\n";
        output << "  <h1>" << kTitlePrefix << "</h1>\n";
        output << "  <div class=\"subtitle\">" << escape_json(suite_name_) << " / " << escape_json(test_name_)
               << "</div>\n";
        output << "</div>\n";
        output << "<div class=\"controls\">\n";
        output << "  <button id=\"start-button\">Start</button>\n";
        output << "  <button id=\"stop-button\" disabled>Stop</button>\n";
        output << "  <button id=\"reset-button\">Reset</button>\n";
        output << "  <input type=\"range\" id=\"timeline-slider\" min=\"0\" value=\"0\" />\n";
        output << "  <span class=\"time-label\" id=\"time-display\">0.000s</span>\n";
        output << "  <label class=\"speed-control\">Speed\n";
        output << "    <select id=\"speed-select\">\n";
        output << "      <option value=\"0.25\">0.25×</option>\n";
        output << "      <option value=\"0.5\">0.5×</option>\n";
        output << "      <option value=\"1\">1×</option>\n";
        output << "      <option value=\"1.5\">1.5×</option>\n";
        output << "      <option value=\"2\">2×</option>\n";
        output << "      <option value=\"5\" selected>5×</option>\n";
        output << "      <option value=\"10\">10×</option>\n";
        output << "      <option value=\"20\">20×</option>\n";
        output << "    </select>\n";
        output << "  </label>\n";
        output << "  <div class=\"type-filters\" id=\"type-filters\">\n";
        output << "    <span class=\"filter-title\">Show</span>\n";
        output << "  </div>\n";
        output << "  <label class=\"filter-item\">\n";
        output << "    <input type=\"checkbox\" id=\"show-log-checkbox\" checked>\n";
        output << "    <span>Show Log</span>\n";
        output << "  </label>\n";
        output << "</div>\n";
        output << "<div id=\"main-layout\">\n";
        output << "  <div id=\"viz-container\"></div>\n";
        output << "  <aside id=\"event-panel\">\n";
        output << "    <div class=\"event-log-header\">\n";
        output << "      <h2>Event Log</h2>\n";
        output << "    </div>\n";
        output << "    <div id=\"event-log-body\">\n";
        output << "      <ul id=\"event-log\"></ul>\n";
        output << "    </div>\n";
        output << "  </aside>\n";
        output << "</div>\n";

        output << "<script>\n";
        output << "const telemetryMeta = { suite: \"" << escape_json(suite_name_) << "\", test: \""
               << escape_json(test_name_) << "\" };\n";

        output << "const events = [\n";
        for (size_t idx = 0; idx < events_copy.size(); ++idx)
        {
            const auto& evt = events_copy[idx];
            output << "  { type: \"" << escape_json(evt.type) << "\", timestamp: " << std::fixed << std::setprecision(6)
                   << evt.timestamp << ", data: {";
            for (size_t field_idx = 0; field_idx < evt.fields.size(); ++field_idx)
            {
                const auto& field = evt.fields[field_idx];
                if (field_idx > 0)
                {
                    output << ", ";
                }
                output << "\"" << field.key << "\": ";
                switch (field.type)
                {
                case field_kind::string:
                    output << "\"" << escape_json(field.value) << "\"";
                    break;
                case field_kind::number:
                    output << field.value;
                    break;
                case field_kind::boolean:
                    output << (field.value == "true" ? "true" : "false");
                    break;
                case field_kind::floating:
                    output << field.value;
                    break;
                }
            }
            output << " } }";
            if (idx + 1 < events_copy.size())
            {
                output << ",";
            }
            output << "\n";
        }
        output << "];\n";
        output << "const totalDuration = " << std::fixed << std::setprecision(6) << total_duration << ";\n";
        output << "</script>\n";

        output << "<script src=\"https://d3js.org/d3.v7.min.js\"></script>\n";
        output << "<script>\n" << kAnimationScriptPart1 << kAnimationScriptPart2 << "\n</script>\n";
        output << "</body>\n</html>\n";
    }

    void animation_telemetry_service::on_transport_creation(
        const std::string& name, rpc::zone zone_id, rpc::zone adjacent_zone_id, rpc::transport_status status) const
    {
        record_event("transport_creation",
            {make_string_field("name", name),
                make_number_field("zone_id", zone_id.get_val()),
                make_number_field("adjacent_zone_id", adjacent_zone_id.get_val()),
                make_number_field("status", static_cast<uint32_t>(status))});
    }

    void animation_telemetry_service::on_transport_deletion(rpc::zone zone_id, rpc::zone adjacent_zone_id) const
    {
        record_event("transport_deletion",
            {make_number_field("zone_id", zone_id.get_val()),
                make_number_field("adjacent_zone_id", adjacent_zone_id.get_val())});
    }

    void animation_telemetry_service::on_transport_status_change(const std::string& name,
        rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::transport_status old_status,
        rpc::transport_status new_status) const
    {
        record_event("transport_status_change",
            {make_string_field("name", name),
                make_number_field("zone_id", zone_id.get_val()),
                make_number_field("adjacent_zone_id", adjacent_zone_id.get_val()),
                make_number_field("old_status", static_cast<uint32_t>(old_status)),
                make_number_field("new_status", static_cast<uint32_t>(new_status))});
    }

    void animation_telemetry_service::on_transport_add_destination(
        rpc::zone zone_id, rpc::zone adjacent_zone_id, rpc::destination_zone destination, rpc::caller_zone caller) const
    {
        record_event("transport_add_destination",
            {make_number_field("zone_id", zone_id.get_val()),
                make_number_field("adjacent_zone_id", adjacent_zone_id.get_val()),
                make_number_field("destination", destination.get_val()),
                make_number_field("caller", caller.get_val())});
    }

    void animation_telemetry_service::on_transport_remove_destination(
        rpc::zone zone_id, rpc::zone adjacent_zone_id, rpc::destination_zone destination, rpc::caller_zone caller) const
    {
        record_event("transport_remove_destination",
            {make_number_field("zone_id", zone_id.get_val()),
                make_number_field("adjacent_zone_id", adjacent_zone_id.get_val()),
                make_number_field("destination", destination.get_val()),
                make_number_field("caller", caller.get_val())});
    }

    void animation_telemetry_service::on_pass_through_creation(rpc::zone zone_id,
        rpc::destination_zone forward_destination,
        rpc::destination_zone reverse_destination,
        uint64_t shared_count,
        uint64_t optimistic_count) const
    {
        record_event("pass_through_creation",
            {make_number_field("zone_id", zone_id.get_val()),
                make_number_field("forward_destination", forward_destination.get_val()),
                make_number_field("reverse_destination", reverse_destination.get_val()),
                make_number_field("shared_count", shared_count),
                make_number_field("optimistic_count", optimistic_count)});
    }

    void animation_telemetry_service::on_pass_through_deletion(
        rpc::zone zone_id, rpc::destination_zone forward_destination, rpc::destination_zone reverse_destination) const
    {
        record_event("pass_through_deletion",
            {make_number_field("zone_id", zone_id.get_val()),
                make_number_field("forward_destination", forward_destination.get_val()),
                make_number_field("reverse_destination", reverse_destination.get_val())});
    }

    void animation_telemetry_service::on_pass_through_add_ref(rpc::zone zone_id,
        rpc::destination_zone forward_destination,
        rpc::destination_zone reverse_destination,
        rpc::add_ref_options options,
        int64_t shared_delta,
        int64_t optimistic_delta) const
    {
        record_event("pass_through_add_ref",
            {make_number_field("zone_id", zone_id.get_val()),
                make_number_field("forward_destination", forward_destination.get_val()),
                make_number_field("reverse_destination", reverse_destination.get_val()),
                make_number_field("options", static_cast<uint64_t>(options)),
                make_signed_field("shared_delta", shared_delta),
                make_signed_field("optimistic_delta", optimistic_delta)});
    }

    void animation_telemetry_service::on_pass_through_release(rpc::zone zone_id,
        rpc::destination_zone forward_destination,
        rpc::destination_zone reverse_destination,
        int64_t shared_delta,
        int64_t optimistic_delta) const
    {
        record_event("pass_through_release",
            {make_number_field("zone_id", zone_id.get_val()),
                make_number_field("forward_destination", forward_destination.get_val()),
                make_number_field("reverse_destination", reverse_destination.get_val()),
                make_signed_field("shared_delta", shared_delta),
                make_signed_field("optimistic_delta", optimistic_delta)});
    }

    void animation_telemetry_service::on_pass_through_status_change(rpc::zone zone_id,
        rpc::destination_zone forward_destination,
        rpc::destination_zone reverse_destination,
        rpc::transport_status forward_status,
        rpc::transport_status reverse_status) const
    {
        record_event("pass_through_status_change",
            {make_number_field("zone_id", zone_id.get_val()),
                make_number_field("forward_destination", forward_destination.get_val()),
                make_number_field("reverse_destination", reverse_destination.get_val()),
                make_number_field("forward_status", static_cast<uint32_t>(forward_status)),
                make_number_field("reverse_status", static_cast<uint32_t>(reverse_status))});
    }

    // Service methods
    void animation_telemetry_service::on_service_send(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        record_event("service_send",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
    }

    void animation_telemetry_service::on_service_post(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        record_event("service_post",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
    }

    void animation_telemetry_service::on_service_object_released(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id) const
    {
        record_event("service_object_released",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val())});
    }

    void animation_telemetry_service::on_service_transport_down(
        rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
    {
        record_event("service_transport_down",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val())});
    }

    // Service proxy methods
    void animation_telemetry_service::on_service_proxy_send(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        record_event("service_proxy_send",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
    }

    void animation_telemetry_service::on_service_proxy_post(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        record_event("service_proxy_post",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
    }

    void animation_telemetry_service::on_service_proxy_object_released(rpc::zone zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id) const
    {
        record_event("service_proxy_object_released",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val())});
    }

    void animation_telemetry_service::on_service_proxy_transport_down(
        rpc::zone zone_id, rpc::destination_zone destination_zone_id, rpc::caller_zone caller_zone_id) const
    {
        record_event("service_proxy_transport_down",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val())});
    }

    // Transport outbound methods
    void animation_telemetry_service::on_transport_outbound_send(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        record_event("transport_outbound_send",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
    }

    void animation_telemetry_service::on_transport_outbound_post(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        record_event("transport_outbound_post",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
    }

    void animation_telemetry_service::on_transport_outbound_try_cast(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        record_event("transport_outbound_try_cast",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val())});
    }

    void animation_telemetry_service::on_transport_outbound_add_ref(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options options,
        uint64_t reference_count) const
    {
        record_event("transport_outbound_add_ref",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("knownDirectionZone", known_direction_zone_id.get_val()),
                make_number_field("options", static_cast<uint64_t>(options)),
                make_number_field("referenceCount", reference_count)});
    }

    void animation_telemetry_service::on_transport_outbound_release(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::release_options options,
        uint64_t reference_count) const
    {
        record_event("transport_outbound_release",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("options", static_cast<uint64_t>(options)),
                make_number_field("referenceCount", reference_count)});
    }

    void animation_telemetry_service::on_transport_outbound_object_released(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id) const
    {
        record_event("transport_outbound_object_released",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val())});
    }

    void animation_telemetry_service::on_transport_outbound_transport_down(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        record_event("transport_outbound_transport_down",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val())});
    }

    // Transport inbound methods
    void animation_telemetry_service::on_transport_inbound_send(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        record_event("transport_inbound_send",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
    }

    void animation_telemetry_service::on_transport_inbound_post(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id) const
    {
        record_event("transport_inbound_post",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val()),
                make_number_field("method", method_id.get_val())});
    }

    void animation_telemetry_service::on_transport_inbound_try_cast(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id) const
    {
        record_event("transport_inbound_try_cast",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("interface", interface_id.get_val())});
    }

    void animation_telemetry_service::on_transport_inbound_add_ref(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options options,
        uint64_t reference_count) const
    {
        record_event("transport_inbound_add_ref",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("knownDirectionZone", known_direction_zone_id.get_val()),
                make_number_field("options", static_cast<uint64_t>(options)),
                make_number_field("referenceCount", reference_count)});
    }

    void animation_telemetry_service::on_transport_inbound_release(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id,
        rpc::release_options options,
        uint64_t reference_count) const
    {
        record_event("transport_inbound_release",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val()),
                make_number_field("options", static_cast<uint64_t>(options)),
                make_number_field("referenceCount", reference_count)});
    }

    void animation_telemetry_service::on_transport_inbound_object_released(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id,
        rpc::object object_id) const
    {
        record_event("transport_inbound_object_released",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val()),
                make_number_field("object", object_id.get_val())});
    }

    void animation_telemetry_service::on_transport_inbound_transport_down(rpc::zone zone_id,
        rpc::zone adjacent_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::caller_zone caller_zone_id) const
    {
        record_event("transport_inbound_transport_down",
            {make_number_field("zone", zone_id.get_val()),
                make_number_field("adjacentZone", adjacent_zone_id.get_val()),
                make_number_field("destinationZone", destination_zone_id.get_val()),
                make_number_field("callerZone", caller_zone_id.get_val())});
    }
}
