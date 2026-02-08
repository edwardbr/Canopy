const TITLE_PREFIX = 'RPC++ Telemetry Animation';

function initAnimationTelemetry() {
    if (window.__animationTelemetryInitialized) {
        return;
    }
    window.__animationTelemetryInitialized = true;
    const meta = typeof telemetryMeta == 'object' && telemetryMeta ? telemetryMeta : {};
    const suite = meta.suite || '';
    const test = meta.test || '';
    const title = document.getElementById('page-title');
    if (title) {
        title.textContent = TITLE_PREFIX;
    }
    const subtitle = document.getElementById('page-subtitle');
    if (subtitle) {
        subtitle.textContent = suite && test ? `${suite} / ${test}` : (suite || test || '');
    }
    const titleSuffix = suite && test ? `${suite}.${test}` : (suite || test);
    if (titleSuffix) {
        document.title = `${TITLE_PREFIX} - ${titleSuffix}`;
    } else {
        document.title = TITLE_PREFIX;
    }

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
    const serviceProxyLineHeight = 12;
    const serviceProxyBoxPaddingX = 8;
    const serviceProxyBoxPaddingY = 6;
    const serviceProxyMinWidth = 60;
    const serviceProxyMinHeight = 30;
    const serviceProxyMaxWidth = 200;

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
    const zonePadding = 20;
    const sectionSpacing = 10;
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
    const flashEdgeSeparator = ':';
    const nodeTypeFilters = [
        { key: 'service_proxy', label: 'Service Proxies', defaultVisible: false },
        { key: 'object_proxy', label: 'Object Proxies', defaultVisible: false },
        { key: 'interface_proxy', label: 'Interface Proxies', defaultVisible: true },
        { key: 'stub', label: 'Stubs', defaultVisible: false },
        { key: 'impl', label: 'Implementations', defaultVisible: true },
        { key: 'passthrough', label: 'Passthroughs', defaultVisible: false },
        { key: 'transport', label: 'Transports', defaultVisible: true },
        { key: 'activity', label: 'Activity Pulses', defaultVisible: false }
    ];
    const typeVisibility = new Map();
    nodeTypeFilters.forEach((filter) => typeVisibility.set(filter.key, filter.defaultVisible));
    const zoneAliases = new Map();
    let primaryZoneId = null;
    let flashHandlersBound = false;

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
                    serviceProxies: [],
                    objectProxies: [],
                    stubs: [],
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
                            serviceProxies: [],
                            objectProxies: [],
                            stubs: [],
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
                        serviceProxies: [],
                        objectProxies: [],
                        stubs: [],
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
                rebuildEventLog();
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
            rebuildEventLog();
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
            if (entry.serviceProxies) {
                entry.serviceProxies = entry.serviceProxies.filter((sp) => sp.destZone !== zoneNumber);
            }
            if (entry.objectProxies) {
                entry.objectProxies = entry.objectProxies.filter((op) => op.destZone !== zoneNumber);
            }
            // stubs don't reference other zones, so no cleanup needed
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
                deleteServiceProxy(evt);
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
                deleteObjectProxy(evt);
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
            case 'service_proxy_send':
                pulseActivity(evt);
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
        // Track service proxy in zones structure for visualization
        if (zones[zoneNumber]) {
            // Ensure serviceProxies array exists
            if (!zones[zoneNumber].serviceProxies) {
                zones[zoneNumber].serviceProxies = [];
            }
            // Check if this service proxy already exists (avoid duplicates)
            const existingProxy = zones[zoneNumber].serviceProxies.find(sp => sp.destZone === destinationZoneNumber);
            if (!existingProxy) {
                zones[zoneNumber].serviceProxies.push({ destZone: destinationZoneNumber });
            }
        }
        appendLog(evt);
    }

    function deleteServiceProxy(evt) {
        const zoneNumber = normalizeZoneNumber(evt.data.zone);
        const destinationZoneNumber = normalizeZoneNumber(evt.data.destinationZone);
        const id = makeServiceProxyId(evt.data);
        // Remove from zones tracking
        if (zones[zoneNumber] && zones[zoneNumber].serviceProxies) {
            zones[zoneNumber].serviceProxies = zones[zoneNumber].serviceProxies.filter(
                sp => sp.destZone !== destinationZoneNumber
            );
        }
        deleteNode(id);
    }

    function deleteObjectProxy(evt) {
        const zoneNumber = normalizeZoneNumber(evt.data.zone);
        const destinationZoneNumber = normalizeZoneNumber(evt.data.destinationZone);
        const objectId = Number(evt.data.object);
        const id = makeObjectProxyId(evt.data);
        // Remove from zones tracking
        if (zones[zoneNumber] && zones[zoneNumber].objectProxies) {
            zones[zoneNumber].objectProxies = zones[zoneNumber].objectProxies.filter(
                op => !(op.destZone === destinationZoneNumber && op.object === objectId)
            );
        }
        deleteNode(id);
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
        const options = Number(evt.data.options) || 0;
        if (evt.type === 'service_proxy_add_ref' && options === 3) {
            pulseRelayActivity(evt, zoneNumber, destinationZoneNumber, 'service_proxy');
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
        // Track stub in zones structure for visualization
        if (zones[zoneNumber]) {
            if (!zones[zoneNumber].stubs) {
                zones[zoneNumber].stubs = [];
            }
            // Check if this stub already exists (avoid duplicates)
            const existingStub = zones[zoneNumber].stubs.find(s => s.object === objectId);
            if (!existingStub) {
                zones[zoneNumber].stubs.push({ object: objectId, address: evt.data.address });
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

        const isAdd = evt.type.includes('add_ref');
        const options = Number(evt.data.options) || 0;
        if (isAdd && options === 3) {
            pulseRelayActivity(evt, zoneNumber, adjacentNumber, 'transport');
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
        // Track object proxy in zones structure for visualization
        if (zones[zoneNumber]) {
            if (!zones[zoneNumber].objectProxies) {
                zones[zoneNumber].objectProxies = [];
            }
            // Check if this object proxy already exists (avoid duplicates)
            const existingProxy = zones[zoneNumber].objectProxies.find(op => op.destZone === destinationZoneNumber && op.object === evt.data.object);
            if (!existingProxy) {
                zones[zoneNumber].objectProxies.push({ destZone: destinationZoneNumber, object: evt.data.object });
            }
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
        const zoneNumber = normalizeZoneNumber(evt.data.zone);
        const objectId = Number(evt.data.object);
        const id = makeStubId(evt.data);
        // Remove from zones tracking
        if (zones[zoneNumber] && zones[zoneNumber].stubs) {
            zones[zoneNumber].stubs = zones[zoneNumber].stubs.filter(
                s => s.object !== objectId
            );
        }
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
            if (transportErrors.has(transportId)) {
                appendTransportAudit(
                    `transport ${transportId} has cleaned itself up`,
                    { transportId });
                transportErrors.delete(transportId);
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
        const forwardDestNumber = normalizeZoneNumber(evt.data.forward_destination);
        const reverseDestNumber = normalizeZoneNumber(evt.data.reverse_destination);
        const node = nodes.get(id);
        if (evt.type === 'pass_through_add_ref') {
            const options = Number(evt.data.options) || 0;
            if (options === 3) {
                pulseRelayActivity(evt, forwardDestNumber, reverseDestNumber, 'passthrough');
                return;
            }
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

    function pulseRelayActivity(evt, sourceZone, targetZone, variant) {
        if (!sourceZone || !targetZone || sourceZone === targetZone) {
            appendLog(evt);
            return;
        }
        const relayId = `relay-${variant}-${evt.type}-${evt.timestamp}-${Math.random()}`;
        const relayLink = {
            id: relayId,
            type: 'relay_activity',
            source: `zone-${sourceZone}`,
            target: `zone-${targetZone}`,
            variant: variant,
            label: evt.type.replace(/_/g, ' '),
            timestamp: evt.timestamp,
            temporary: true
        };
        links.set(relayId, relayLink);
        setTimeout(() => {
            links.delete(relayId);
            updateGraph();
        }, 2000);
        updateGraph();
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

    function eventTypeToFilterKey(type) {
        if (type === 'cloned_service_proxy_creation' || type.startsWith('service_proxy_')) {
            return 'service_proxy';
        }
        if (type.startsWith('object_proxy_')) {
            return 'object_proxy';
        }
        if (type.startsWith('interface_proxy_')) {
            return 'interface_proxy';
        }
        if (type.startsWith('stub_')) {
            return 'stub';
        }
        if (type.startsWith('impl_')) {
            return 'impl';
        }
        if (type.startsWith('pass_through_')) {
            return 'passthrough';
        }
        if (type.startsWith('transport_') || type === 'transport_ref_audit') {
            return 'transport';
        }
        return null;
    }

    function shouldShowEvent(evt) {
        const key = eventTypeToFilterKey(evt.type || '');
        if (!key) {
            return true;
        }
        return typeVisibility.get(key);
    }

    function appendLog(evt, options = {}) {
        if (!shouldShowEvent(evt)) {
            return;
        }
        const li = document.createElement('li');
        const time = evt.timestamp.toFixed(3).padStart(7, ' ');
        const eventType = evt.type === 'interface_proxy_send' ? `<strong>${evt.type}</strong>` : evt.type;
        if (evt.type === 'interface_proxy_send') {
            li.classList.add('interface-proxy-send');
        }
        // Check for deletion/destruction events
        if (evt.type.includes('deletion') || evt.type.includes('destruction')) {
            li.classList.add('deletion-event');
        }
        // Check for error messages
        const eventSummary = formatEventSummary(evt);
        if (evt.type === 'transport_ref_audit' && evt.data && evt.data.message) {
            const msg = evt.data.message.toLowerCase();
            if (msg.includes('still alive with zero ref counts') ||
                msg.includes('went negative') ||
                msg.includes('negative') ||
                msg.includes('error')) {
                li.classList.add('error-message');
            }
        }
        li.innerHTML = `<div class="timestamp">${time}s · ${eventType}</div><div>${eventSummary}</div>`;
        eventLog.appendChild(li);
        while (eventLog.childNodes.length > maxLogEntries) {
            eventLog.removeChild(eventLog.firstChild);
        }
        if (eventLogBody && !logCollapsed && !options.skipScroll) {
            eventLogBody.scrollTop = eventLogBody.scrollHeight;
        }
    }

    function clearLog() {
        eventLog.textContent = '';
    }

    function rebuildEventLog() {
        if (!eventLog) {
            return;
        }
        clearLog();
        if (processedIndex <= 0) {
            return;
        }
        for (let i = 0; i < processedIndex; i += 1) {
            appendLog(events[i], { skipScroll: true });
        }
        if (eventLogBody && !logCollapsed) {
            eventLogBody.scrollTop = eventLogBody.scrollHeight;
        }
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
        // Only show title row, no destination/source reference count rows
        return [header];
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

    function buildServiceProxyLines(zoneNumber, destinationZoneNumber) {
        const lines = [`TO:${destinationZoneNumber}`];
        // Find service proxy node to get ref counts
        // Service proxies may have different callerZone values, so we need to search for any matching proxy
        let totalRefCount = 0;
        let totalExtCount = 0;
        let foundAny = false;

        // Check all possible caller zone values
        nodes.forEach((node) => {
            if (node.type === 'service_proxy' &&
                node.zone === zoneNumber &&
                node.destinationZone === destinationZoneNumber) {
                totalRefCount += (node.refCount || 0);
                totalExtCount += (node.externalRefCount || 0);
                foundAny = true;
            }
        });

        if (foundAny) {
            lines.push(`R${totalRefCount} E${totalExtCount}`);
        } else {
            lines.push('no refs');
        }
        return lines;
    }

    function computeServiceProxyMetrics(zoneNumber, destinationZoneNumber) {
        const lines = buildServiceProxyLines(zoneNumber, destinationZoneNumber);
        const maxLen = lines.reduce((max, line) => Math.max(max, line.length), 0);
        const width = Math.min(
            serviceProxyMaxWidth,
            Math.max(serviceProxyMinWidth, serviceProxyBoxPaddingX * 2 + maxLen * 6));
        const height = Math.max(
            serviceProxyMinHeight,
            serviceProxyBoxPaddingY * 2 + lines.length * serviceProxyLineHeight);
        return { lines, width, height };
    }

    function buildObjectProxyLines(zoneNumber, destinationZoneNumber, objectId) {
        const lines = [`OBJ:${objectId}`, `TO:${destinationZoneNumber}`];
        return lines;
    }

    function computeObjectProxyMetrics(zoneNumber, destinationZoneNumber, objectId) {
        const lines = buildObjectProxyLines(zoneNumber, destinationZoneNumber, objectId);
        const maxLen = lines.reduce((max, line) => Math.max(max, line.length), 0);
        const width = Math.min(
            serviceProxyMaxWidth,
            Math.max(serviceProxyMinWidth, serviceProxyBoxPaddingX * 2 + maxLen * 6));
        const height = Math.max(
            serviceProxyMinHeight,
            serviceProxyBoxPaddingY * 2 + lines.length * serviceProxyLineHeight);
        return { lines, width, height };
    }

    function buildStubLines(objectId, address) {
        const lines = [`OBJ:${objectId}`];
        if (address !== undefined && address !== null) {
            lines.push(`@${address.toString(16)}`);
        }
        return lines;
    }

    function computeStubMetrics(objectId, address) {
        const lines = buildStubLines(objectId, address);
        const maxLen = lines.reduce((max, line) => Math.max(max, line.length), 0);
        const width = Math.min(
            serviceProxyMaxWidth,
            Math.max(serviceProxyMinWidth, serviceProxyBoxPaddingX * 2 + maxLen * 6));
        const height = Math.max(
            serviceProxyMinHeight,
            serviceProxyBoxPaddingY * 2 + lines.length * serviceProxyLineHeight);
        return { lines, width, height };
    }

    function resolveSectionWidths(zone, layoutBoxWidth) {
        const passthroughCount = zone && zone.passthroughs ? zone.passthroughs.length : 0;
        if (!typeVisibility.get('passthrough') || passthroughCount === 0) {
            return { leftBoxWidth: layoutBoxWidth, rightBoxWidth: 0 };
        }
        const desiredLeft = Math.max(120, zone.calculatedLeftWidth || 0);
        const desiredRight = Math.max(100, zone.calculatedRightWidth || 0);
        const available = Math.max(layoutBoxWidth - sectionSpacing, 0);
        let left = desiredLeft;
        let right = desiredRight;
        const totalDesired = left + right;

        if (totalDesired > available && totalDesired > 0) {
            const scale = available / totalDesired;
            left *= scale;
            right *= scale;
        } else if (totalDesired < available && totalDesired > 0) {
            const extra = available - totalDesired;
            const leftRatio = left / totalDesired;
            left += extra * leftRatio;
            right += extra * (1 - leftRatio);
        } else if (totalDesired === 0) {
            left = available * 0.6;
            right = available * 0.4;
        }

        return { leftBoxWidth: left, rightBoxWidth: right };
    }

    function rebuildVisualization() {
        const showTransports = typeVisibility.get('transport');
        const showPassthroughs = typeVisibility.get('passthrough');
        const showServiceProxies = typeVisibility.get('service_proxy');
        const showObjectProxies = typeVisibility.get('object_proxy');
        const showStubs = typeVisibility.get('stub');
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
            serviceProxies: [],
            objectProxies: [],
            stubs: [],
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
            const adjacentIds = new Set();
            if (showTransports) {
                z.transports.forEach((t) => adjacentIds.add(t.adjId));
                if (z.parentId && z.parentId !== 0) {
                    adjacentIds.add(z.parentId);
                }
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
            if (showPassthroughs) {
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
            }
            z.passthroughBoxWidth = maxPassthroughBoxWidth;
            z.passthroughBoxHeight = maxPassthroughBoxHeight;

            // Calculate service proxy metrics (include object proxies for sizing)
            z.serviceProxyMetrics = [];
            let maxServiceProxyBoxWidth = serviceProxyMinWidth;
            let maxServiceProxyBoxHeight = serviceProxyMinHeight;
            if (showServiceProxies && z.serviceProxies && z.serviceProxies.length > 0) {
                z.serviceProxies.forEach((sp) => {
                    const metrics = computeServiceProxyMetrics(z.id, sp.destZone);
                    let expandedWidth = metrics.width;
                    let expandedHeight = metrics.height;

                    if (showObjectProxies && z.objectProxies && z.objectProxies.length > 0) {
                        const objectProxiesForThisService = z.objectProxies.filter(op => op.destZone === sp.destZone);
                        if (objectProxiesForThisService.length > 0) {
                            const objectProxySpacing = 15;
                            let maxObjectProxyWidth = 0;
                            const totalObjectProxyHeight = objectProxiesForThisService.reduce((sum, op, idx) => {
                                const opMetrics = computeObjectProxyMetrics(z.id, op.destZone, op.object);
                                maxObjectProxyWidth = Math.max(maxObjectProxyWidth, opMetrics.width);
                                return sum + opMetrics.height + (idx > 0 ? objectProxySpacing : 0);
                            }, 0);
                            expandedHeight = metrics.height + totalObjectProxyHeight + 30;
                            expandedWidth = Math.max(expandedWidth, maxObjectProxyWidth + 20);
                        }
                    }

                    metrics.expandedWidth = expandedWidth;
                    metrics.expandedHeight = expandedHeight;
                    z.serviceProxyMetrics.push(metrics);
                    maxServiceProxyBoxWidth = Math.max(maxServiceProxyBoxWidth, expandedWidth);
                    maxServiceProxyBoxHeight = Math.max(maxServiceProxyBoxHeight, expandedHeight);
                });
            }
            z.serviceProxyBoxWidth = maxServiceProxyBoxWidth;
            z.serviceProxyBoxHeight = maxServiceProxyBoxHeight;

            // Calculate object proxy metrics
            z.objectProxyMetrics = [];
            let maxObjectProxyBoxWidth = serviceProxyMinWidth;
            let maxObjectProxyBoxHeight = serviceProxyMinHeight;
            if (showObjectProxies && z.objectProxies && z.objectProxies.length > 0) {
                z.objectProxies.forEach((op) => {
                    const metrics = computeObjectProxyMetrics(z.id, op.destZone, op.object);
                    z.objectProxyMetrics.push(metrics);
                    maxObjectProxyBoxWidth = Math.max(maxObjectProxyBoxWidth, metrics.width);
                    maxObjectProxyBoxHeight = Math.max(maxObjectProxyBoxHeight, metrics.height);
                });
            }
            z.objectProxyBoxWidth = maxObjectProxyBoxWidth;
            z.objectProxyBoxHeight = maxObjectProxyBoxHeight;

            // Calculate stub metrics
            z.stubMetrics = [];
            let maxStubBoxWidth = serviceProxyMinWidth;
            let maxStubBoxHeight = serviceProxyMinHeight;
            if (showStubs && z.stubs && z.stubs.length > 0) {
                z.stubs.forEach((s) => {
                    const metrics = computeStubMetrics(s.object, s.address);
                    z.stubMetrics.push(metrics);
                    maxStubBoxWidth = Math.max(maxStubBoxWidth, metrics.width);
                    maxStubBoxHeight = Math.max(maxStubBoxHeight, metrics.height);
                });
            }
            z.stubBoxWidth = maxStubBoxWidth;
            z.stubBoxHeight = maxStubBoxHeight;

            // Calculate zone width based on actual layout sections
            // Left box (60%): service proxies and stubs share space
            // Right box (40%): passthroughs
            // Transports are at top/bottom and use full width

            const transportCount = showTransports ? z.transports.length : 0;
            const passthroughCount = showPassthroughs ? z.passthroughs.length : 0;
            const serviceProxyCount = showServiceProxies && z.serviceProxies ? z.serviceProxies.length : 0;
            const objectProxyCount = showObjectProxies && z.objectProxies ? z.objectProxies.length : 0;
            const stubCount = showStubs && z.stubs ? z.stubs.length : 0;

            // Calculate columns needed for left section (service proxies and stubs)
            // They use a sqrt-based grid layout
            const leftSectionMaxWidth = Math.max(maxServiceProxyBoxWidth, maxStubBoxWidth);
            const leftMaxCount = Math.max(serviceProxyCount, stubCount);
            const leftCols = leftMaxCount > 0 ? Math.ceil(Math.sqrt(leftMaxCount)) : 0;
            const leftSectionWidth = leftCols > 0
                ? leftCols * (leftSectionMaxWidth + 15) - 15 + 40  // 40px padding
                : 120;  // Minimum left section width

            // Calculate width needed for right section (passthroughs)
            // Allow passthroughs to grow horizontally to avoid vertical stacking
            // Target: fit as many as possible in a row, up to a reasonable max
            const rightMaxWidth = maxPassthroughBoxWidth;
            const passthroughSpacing = 15;
            const minRightWidth = 100;  // Minimum right section width
            const maxPassthroughsPerRow = 4;  // Cap to prevent excessive width

            let rightSectionWidth = 0;
            if (passthroughCount > 0) {
                rightSectionWidth = minRightWidth;
                // Calculate how many passthroughs we'd like per row (up to max)
                const desiredPerRow = Math.min(passthroughCount, maxPassthroughsPerRow);
                // Calculate width needed for that many passthroughs
                rightSectionWidth = desiredPerRow * (rightMaxWidth + passthroughSpacing) - passthroughSpacing + 40;  // 40px padding
            }

            // Transports at top need width too
            const transportSectionWidth = transportCount > 0
                ? transportCount * (maxTransportBoxWidth + 20) - 20 + 80  // 80px padding
                : 100;

            // Total width is the max of: left+right+spacing, transport section, or minimum
            const sectionGap = rightSectionWidth > 0 ? sectionSpacing : 0;
            const contentWidth = Math.max(
                leftSectionWidth + rightSectionWidth + sectionGap,
                transportSectionWidth
            );

            z.width = Math.max(260, contentWidth + zonePadding * 2);

            z.calculatedLeftWidth = leftSectionWidth;
            z.calculatedRightWidth = rightSectionWidth;

            // Calculate height to fully contain left/right content
            const serviceBoxHeight = 40;
            const contentSpacing = 40;
            const stubSpacing = 15;
            const proxySpacing = 15;
            const passthroughRowSpacing = 15;

            const layoutBoxWidth = z.width - zonePadding * 2;
            const { leftBoxWidth, rightBoxWidth } = resolveSectionWidths(z, layoutBoxWidth);

            let leftContentHeight = serviceBoxHeight;
            if (stubCount > 0) {
                const stubsPerRow = Math.ceil(Math.sqrt(z.stubs.length));
                const stubRows = Math.ceil(z.stubs.length / stubsPerRow);
                const stubsHeight = stubRows * z.stubBoxHeight + (stubRows - 1) * stubSpacing;
                leftContentHeight += contentSpacing + stubsHeight;
            }

            if (serviceProxyCount > 0) {
                const leftSectionPadding = 40;
                const availableWidth = leftBoxWidth - leftSectionPadding;
                let proxiesPerRow = Math.ceil(Math.sqrt(z.serviceProxies.length));
                const minBoxWidthWithSpacing = z.serviceProxyBoxWidth + proxySpacing;
                const maxPossiblePerRow = Math.max(1, Math.floor(availableWidth / minBoxWidthWithSpacing));
                proxiesPerRow = Math.min(proxiesPerRow, maxPossiblePerRow);
                const proxyRows = Math.ceil(z.serviceProxies.length / proxiesPerRow);

                let serviceProxiesHeight = 0;
                for (let r = 0; r < proxyRows; r++) {
                    const startIdx = r * proxiesPerRow;
                    const endIdx = Math.min(startIdx + proxiesPerRow, z.serviceProxies.length);
                    let maxRowHeight = 0;
                    for (let i = startIdx; i < endIdx; i++) {
                        const metrics = z.serviceProxyMetrics[i] || computeServiceProxyMetrics(z.id, z.serviceProxies[i].destZone);
                        const rowHeight = metrics.expandedHeight || metrics.height;
                        maxRowHeight = Math.max(maxRowHeight, rowHeight);
                    }
                    serviceProxiesHeight += maxRowHeight;
                    if (r < proxyRows - 1) {
                        serviceProxiesHeight += proxySpacing;
                    }
                }
                leftContentHeight += contentSpacing + serviceProxiesHeight;
            }

            let rightContentHeight = 0;
            if (passthroughCount > 0) {
                const maxPassthroughWidth = Math.max(...z.passthroughs.map((p, i) => {
                    const metrics = z.passthroughMetrics[i] || computePassthroughMetrics(p.fwd, p.rev, 0, 0);
                    return metrics.width;
                }), passthroughMinWidth);
                const passthroughsPerRow = Math.max(1, Math.floor((rightBoxWidth + passthroughRowSpacing) / (maxPassthroughWidth + passthroughRowSpacing)));
                const passthroughRows = Math.ceil(z.passthroughs.length / passthroughsPerRow);
                let rowHeights = new Array(passthroughRows).fill(0);
                z.passthroughs.forEach((p, i) => {
                    const row = Math.floor(i / passthroughsPerRow);
                    const metrics = z.passthroughMetrics[i] || computePassthroughMetrics(p.fwd, p.rev, 0, 0);
                    rowHeights[row] = Math.max(rowHeights[row], metrics.height);
                });
                rightContentHeight = rowHeights.reduce((sum, h) => sum + h, 0) + (passthroughRows - 1) * passthroughRowSpacing;
            }

            const transportTopHeight = showTransports && z.transports.length > 0 ? z.transportBoxHeight + 30 : 30;
            const transportBottomHeight = showTransports && z.transports.length > 0 ? z.transportBoxHeight + 30 : 30;
            const layoutPadding = 10;
            const layoutContentHeight = Math.max(leftContentHeight, rightContentHeight, 120) + layoutPadding;
            z.height = Math.max(160, transportTopHeight + transportBottomHeight + layoutContentHeight);
        });

        // Apply tree layout with dynamic vertical spacing based on zone heights
        const maxZWidth = d3.max(Object.values(zones), z => z.width) || 260;
        const maxZoneHeight = d3.max(Object.values(zones), z => z.height) || 200;
        const verticalSpacing = maxZoneHeight + 80; // Zone height plus padding between zones (reduced from 100)
        d3.tree().nodeSize([maxZWidth + 150, verticalSpacing])(root);

        // Normalize vertical spacing so zone-to-zone gaps are consistent regardless of height.
        const zoneGap = 80;
        root.y = 0;
        root.each((node) => {
            if (!node.parent) {
                return;
            }
            const parentHeight = node.parent.data && node.parent.data.data ? node.parent.data.data.height : 0;
            node.y = node.parent.y + parentHeight + zoneGap;
        });

        // Calculate bounding box of all nodes in tree coordinate space
        const descendants = root.descendants();
        const xExtent = d3.extent(descendants, d => d.x);
        const yExtent = d3.extent(descendants, d => d.y);

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
            if (showTransports && d.parent && d.parent.data.id !== 0) {
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
            if (showTransports) {
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
            }
        });

        const zonePositions = new Map();
        root.descendants().filter(d => d.data.id !== 0).forEach(d => {
            zonePositions.set(d.data.id, { x: getX(d), y: getY(d) });
        });

        // Build a map of hierarchical parent-child relationships
        const hierarchicalPairs = new Set();
        root.descendants().filter(d => d.parent && d.data.id !== 0 && d.parent.data.id !== 0).forEach(d => {
            hierarchicalPairs.add(`${d.parent.data.id}-${d.data.id}`);
            hierarchicalPairs.add(`${d.data.id}-${d.parent.data.id}`);
        });

        // Draw peer-to-peer transport links (for zones without hierarchical parent-child relationship)
        if (showTransports) {
            const peerTransportLinks = Array.from(links.values())
                .filter((link) => link.type === 'transport_link')
                .map((link) => {
                    // Extract zone numbers from transport node IDs
                    const sourceNode = nodes.get(link.source);
                    const targetNode = nodes.get(link.target);
                    if (!sourceNode || !targetNode) {
                        return null;
                    }
                    const sourceZone = sourceNode.zone;
                    const targetZone = targetNode.zone;
                    const sourceAdj = sourceNode.adjacentZone;
                    const targetAdj = targetNode.adjacentZone;

                    // Skip if this is a hierarchical connection
                    if (hierarchicalPairs.has(`${sourceZone}-${targetZone}`)) {
                        return null;
                    }

                    // Get zone center positions and zone data
                    const sourcePos = zonePositions.get(sourceZone);
                    const targetPos = zonePositions.get(targetZone);
                    const sourceZoneData = zones[sourceZone];
                    const targetZoneData = zones[targetZone];

                    if (!sourcePos || !targetPos || !sourceZoneData || !targetZoneData) {
                        return null;
                    }

                    // Calculate which sides of the zones face each other
                    const dx = targetPos.x - sourcePos.x;
                    const dy = targetPos.y - sourcePos.y;

                    // Determine if zones are more horizontally or vertically separated
                    const absX = Math.abs(dx);
                    const absY = Math.abs(dy);

                    // Zone coordinate system: center (0,0) is at bottom-center of zone box
                    // Zone box extends from x: -width/2 to +width/2, y: -height to 0
                    let sourceRelX, sourceRelY, targetRelX, targetRelY;

                    if (absX > absY) {
                        // Horizontally separated - use left/right sides at vertical middle
                        const sourceMiddleY = -sourceZoneData.height / 2;
                        const targetMiddleY = -targetZoneData.height / 2;

                        if (dx > 0) {
                            // Target is to the right of source
                            sourceRelX = sourceZoneData.width / 2;
                            sourceRelY = sourceMiddleY;
                            targetRelX = -targetZoneData.width / 2;
                            targetRelY = targetMiddleY;
                        } else {
                            // Target is to the left of source
                            sourceRelX = -sourceZoneData.width / 2;
                            sourceRelY = sourceMiddleY;
                            targetRelX = targetZoneData.width / 2;
                            targetRelY = targetMiddleY;
                        }
                    } else {
                        // Vertically separated - use top/bottom sides at horizontal center
                        if (dy > 0) {
                            // Target is below source (higher Y in SVG)
                            sourceRelX = 0;
                            sourceRelY = 0; // Bottom edge
                            targetRelX = 0;
                            targetRelY = -targetZoneData.height; // Top edge
                        } else {
                            // Target is above source (lower Y in SVG)
                            sourceRelX = 0;
                            sourceRelY = -sourceZoneData.height; // Top edge
                            targetRelX = 0;
                            targetRelY = 0; // Bottom edge
                        }
                    }

                    // Calculate absolute positions
                    const sourceAbsX = sourcePos.x + sourceRelX;
                    const sourceAbsY = sourcePos.y + sourceRelY;
                    const targetAbsX = targetPos.x + targetRelX;
                    const targetAbsY = targetPos.y + targetRelY;

                    // Update PortRegistry with peer-facing positions
                    const sourcePort = PortRegistry[`${sourceZone}:${sourceAdj}`];
                    const targetPort = PortRegistry[`${targetZone}:${targetAdj}`];

                    if (sourcePort && targetPort) {
                        sourcePort.relX = sourceRelX;
                        sourcePort.relY = sourceRelY;
                        sourcePort.absX = sourceAbsX;
                        sourcePort.absY = sourceAbsY;

                        targetPort.relX = targetRelX;
                        targetPort.relY = targetRelY;
                        targetPort.absX = targetAbsX;
                        targetPort.absY = targetAbsY;
                    }

                    return {
                        id: link.id,
                        sourceZone: sourceZone,
                        targetZone: targetZone,
                        x1: sourceAbsX,
                        y1: sourceAbsY,
                        x2: targetAbsX,
                        y2: targetAbsY
                    };
                })
                .filter(Boolean);

            g.selectAll('.peer-trunk-line')
                .data(peerTransportLinks, (d) => d.id)
                .enter()
                .append('line')
                .attr('class', 'peer-trunk-line trunk-line')
                .attr('data-source-zone', d => d.sourceZone)
                .attr('data-target-zone', d => d.targetZone)
                .attr('x1', d => d.x1)
                .attr('y1', d => d.y1)
                .attr('x2', d => d.x2)
                .attr('y2', d => d.y2);

            // Fix hierarchical transport positions in zones that also have peer transports
            // These may have been distributed horizontally, but should be centered
            const affectedZones = new Set();
            peerTransportLinks.forEach(link => {
                affectedZones.add(link.sourceZone);
                affectedZones.add(link.targetZone);
            });

            affectedZones.forEach(zoneNum => {
                const zoneData = zones[zoneNum];
                const zonePos = zonePositions.get(zoneNum);
                if (!zoneData || !zonePos) return;

                // Find hierarchical OUT ports for this zone (child connections going down/out)
                const hierarchicalOutPorts = [];
                zoneData.transports.forEach((t) => {
                    const pairKey = `${zoneNum}-${t.adjId}`;
                    // Only include hierarchical connections, not peer-to-peer
                    if (hierarchicalPairs.has(pairKey)) {
                        const portKey = `${zoneNum}:${t.adjId}`;
                        const port = PortRegistry[portKey];
                        if (port && port.relY === -zoneData.height) { // OUT ports have relY at top edge
                            hierarchicalOutPorts.push({ port, adjId: t.adjId });
                        }
                    }
                });

                // Recalculate horizontal distribution for hierarchical OUT ports only
                if (hierarchicalOutPorts.length > 0) {
                    hierarchicalOutPorts.forEach((item, i) => {
                        const tx = (hierarchicalOutPorts.length > 1)
                            ? (i / (hierarchicalOutPorts.length - 1) * (zoneData.width - 140)) - (zoneData.width / 2 - 70)
                            : 0; // Single port: centered

                        item.port.relX = tx;
                        item.port.absX = zonePos.x + tx;
                        // relY and absY remain unchanged
                    });
                }
            });

            // Now draw all trunk lines after PortRegistry has been fully updated
            // Draw hierarchical trunk lines
            g.selectAll('.hierarchical-trunk-line')
                .data(root.descendants().filter(d => d.parent && d.data.id !== 0 && d.parent.data.id !== 0))
                .enter().append('line')
                .attr('class', 'hierarchical-trunk-line trunk-line')
                .attr('data-source-zone', d => d.parent.data.id)
                .attr('data-target-zone', d => d.data.id)
                .attr('x1', d => PortRegistry[`${d.parent.data.id}:${d.data.id}`].absX)
                .attr('y1', d => PortRegistry[`${d.parent.data.id}:${d.data.id}`].absY)
                .attr('x2', d => PortRegistry[`${d.data.id}:${d.parent.data.id}`].absX)
                .attr('y2', d => PortRegistry[`${d.data.id}:${d.parent.data.id}`].absY);
        }

        const getZoneNumberFromEndpoint = (endpoint) => {
            const id = resolveNodeId(endpoint);
            if (typeof id === 'number') {
                return id;
            }
            if (typeof id === 'string' && id.startsWith('zone-')) {
                const number = Number(id.slice(5));
                return Number.isFinite(number) ? number : null;
            }
            return null;
        };

        const showActivity = typeVisibility.get('activity');
        if (showActivity) {
            const activityLinks = Array.from(links.values())
                .filter((link) => link.type === 'activity' || link.type === 'relay_activity')
                .map((link) => {
                    const sourceZone = getZoneNumberFromEndpoint(link.source);
                    const targetZone = getZoneNumberFromEndpoint(link.target);
                    if (sourceZone === null || targetZone === null) {
                        return null;
                    }
                    const sourcePos = zonePositions.get(sourceZone);
                    const targetPos = zonePositions.get(targetZone);
                    if (!sourcePos || !targetPos) {
                        return null;
                    }
                    const variant = link.type === 'relay_activity'
                        ? (link.variant || 'relay')
                        : 'default';
                    return {
                        id: link.id,
                        x1: sourcePos.x,
                        y1: sourcePos.y,
                        x2: targetPos.x,
                        y2: targetPos.y,
                        variant: variant
                    };
                })
                .filter(Boolean);

            g.selectAll('.activity-line')
                .data(activityLinks, (d) => d.id)
                .enter()
                .append('line')
                .attr('class', (d) => `activity-line ${d.variant}`)
                .attr('x1', (d) => d.x1)
                .attr('y1', (d) => d.y1)
                .attr('x2', (d) => d.x2)
                .attr('y2', (d) => d.y2);
        }

        // Draw zones with internal circuitry (skip virtual root zone 0)
        const nodeGroups = g.selectAll('.node')
            .data(root.descendants().filter(d => d.data.id !== 0))
            .enter().append('g')
            .attr('transform', d => `translate(${getX(d)},${getY(d)})`);

        nodeGroups.each(function (d) {
            const zoneSel = d3.select(this);
            const z = d.data.data;

            // Zone background
            zoneSel.append('rect')
                .attr('class', 'zone-bg')
                .attr('x', -z.width / 2)
                .attr('y', -z.height)
                .attr('width', z.width)
                .attr('height', z.height)
                .attr('rx', 10);

            // Calculate inner layout box dimensions (avoiding transport boxes and zone edges)
            const transportTopHeight = showTransports && z.transports.length > 0 ? z.transportBoxHeight + 30 : 30;
            const transportBottomHeight = showTransports && z.transports.length > 0 ? z.transportBoxHeight + 30 : 30;

            const layoutBoxX = -z.width / 2 + zonePadding;
            const layoutBoxY = -z.height + transportTopHeight;
            const layoutBoxWidth = z.width - 2 * zonePadding;
            const layoutBoxHeight = z.height - transportTopHeight - transportBottomHeight;

            const { leftBoxWidth, rightBoxWidth } = resolveSectionWidths(z, layoutBoxWidth);
            const boxSpacing = rightBoxWidth > 0 ? sectionSpacing : 0;

            // Left box area
            const leftBoxX = layoutBoxX;
            const leftBoxY = layoutBoxY;

            // Right box area
            const rightBoxX = layoutBoxX + leftBoxWidth + boxSpacing;
            const rightBoxY = layoutBoxY;

            // === LEFT BOX CONTENT ===
            // Calculate total content height for vertical centering
            const serviceBoxWidth = 80;  // Increased from 60 for more margin
            const serviceBoxHeight = 40; // Increased from 30 for more margin
            const contentSpacing = 40; // Spacing between service, stubs, and service proxies

            let totalContentHeight = serviceBoxHeight;

            // Calculate stubs height
            let stubsHeight = 0;
            const stubSpacing = 15; // Match passthrough spacing
            if (showStubs && z.stubs && z.stubs.length > 0) {
                const stubsPerRow = Math.ceil(Math.sqrt(z.stubs.length));
                const stubRows = Math.ceil(z.stubs.length / stubsPerRow);
                stubsHeight = stubRows * z.stubBoxHeight + (stubRows - 1) * stubSpacing;
                totalContentHeight += contentSpacing + stubsHeight;
            }

            // Calculate service proxies height (including expanded boxes for object proxies)
            let serviceProxiesHeight = 0;
            let serviceProxyRowHeights = []; // Store actual height for each row
            const proxySpacing = 15; // Match passthrough spacing
            if (showServiceProxies && z.serviceProxies && z.serviceProxies.length > 0) {
                const proxiesPerRow = Math.ceil(Math.sqrt(z.serviceProxies.length));
                const proxyRows = Math.ceil(z.serviceProxies.length / proxiesPerRow);

                // Find max height in each row (accounting for object proxies)
                let maxHeightPerRow = new Array(proxyRows).fill(0);
                z.serviceProxies.forEach((sp, i) => {
                    const row = Math.floor(i / proxiesPerRow);
                    const metrics = z.serviceProxyMetrics[i] || computeServiceProxyMetrics(z.id, sp.destZone);
                    const boxHeight = metrics.expandedHeight || metrics.height;
                    maxHeightPerRow[row] = Math.max(maxHeightPerRow[row], boxHeight);
                });

                serviceProxyRowHeights = maxHeightPerRow;
                serviceProxiesHeight = maxHeightPerRow.reduce((sum, h) => sum + h, 0) + (proxyRows - 1) * proxySpacing;
                totalContentHeight += contentSpacing + serviceProxiesHeight;
            }

            // Center all content in the left box
            const contentStartY = leftBoxY + (layoutBoxHeight - totalContentHeight) / 2;

            const serviceX = leftBoxX + leftBoxWidth / 2;
            const serviceY = contentStartY + serviceBoxHeight / 2;

            zoneSel.append('rect')
                .attr('class', 'service-box')
                .attr('x', serviceX - serviceBoxWidth / 2)
                .attr('y', serviceY - serviceBoxHeight / 2)
                .attr('width', serviceBoxWidth)
                .attr('height', serviceBoxHeight);

            zoneSel.append('text')
                .attr('class', 'zone-label')
                .attr('x', serviceX)
                .attr('y', serviceY + 5)
                .attr('text-anchor', 'middle')
                .text(`${z.name.toUpperCase()} [${z.id}]`);

            let currentY = serviceY + serviceBoxHeight / 2 + contentSpacing;

            // Stubs below service
            if (showStubs && z.stubs && z.stubs.length > 0) {
                const stubsPerRow = Math.ceil(Math.sqrt(z.stubs.length));
                z.stubs.forEach((s, i) => {
                    const row = Math.floor(i / stubsPerRow);
                    const col = i % stubsPerRow;
                    const totalRowWidth = stubsPerRow * (z.stubBoxWidth + stubSpacing) - stubSpacing;
                    const sx = leftBoxX + (leftBoxWidth - totalRowWidth) / 2 + col * (z.stubBoxWidth + stubSpacing) + z.stubBoxWidth / 2;
                    const sy = currentY + row * (z.stubBoxHeight + stubSpacing);

                    const metrics = z.stubMetrics[i] || computeStubMetrics(s.object, s.address);
                    const boxWidth = metrics.width;
                    const boxHeight = metrics.height;
                    const lines = metrics.lines;

                    const sG = zoneSel.append('g').attr('transform', `translate(${sx},${sy})`);

                    sG.append('rect')
                        .attr('class', 'stub-box')
                        .attr('data-object', s.object)
                        .attr('data-zone', z.id)
                        .attr('x', -boxWidth / 2)
                        .attr('y', -boxHeight / 2)
                        .attr('width', boxWidth)
                        .attr('height', boxHeight)
                        .attr('rx', 4);

                    const textStartX = -boxWidth / 2 + serviceProxyBoxPaddingX;
                    const textStartY = -boxHeight / 2 + serviceProxyBoxPaddingY + 9;
                    lines.forEach((line, idx) => {
                        sG.append('text')
                            .attr('class', idx === 0 ? 'stub-label' : 'stub-detail')
                            .attr('x', textStartX)
                            .attr('y', textStartY + idx * serviceProxyLineHeight)
                            .attr('text-anchor', 'start')
                            .text(line);
                    });

                    // Wire from service to stub
                    zoneSel.append('line')
                        .attr('class', 'wire stub-link')
                        .attr('data-object', s.object)
                        .attr('data-zone', z.id)
                        .attr('x1', sx)
                        .attr('y1', sy - boxHeight / 2)
                        .attr('x2', serviceX)
                        .attr('y2', serviceY + serviceBoxHeight / 2);
                });
                const stubRows = Math.ceil(z.stubs.length / stubsPerRow);
                currentY += stubRows * (z.stubBoxHeight + stubSpacing) + contentSpacing;
            }

            // Service proxies below stubs - use left section width calculation for positioning
            if (showServiceProxies && z.serviceProxies && z.serviceProxies.length > 0) {
                // Calculate how many proxies can fit in the left section width (with padding)
                const leftSectionPadding = 40; // Account for box padding
                const availableWidth = leftBoxWidth - leftSectionPadding;
                let proxiesPerRow = Math.ceil(Math.sqrt(z.serviceProxies.length));
                const minBoxWidthWithSpacing = z.serviceProxyBoxWidth + proxySpacing;
                const maxPossiblePerRow = Math.max(1, Math.floor(availableWidth / minBoxWidthWithSpacing));
                proxiesPerRow = Math.min(proxiesPerRow, maxPossiblePerRow);

                // Calculate actual rows
                const proxyRows = Math.ceil(z.serviceProxies.length / proxiesPerRow);

                // Calculate total height accounting for variable rows
                serviceProxiesHeight = 0;
                serviceProxyRowHeights = [];
                for (let r = 0; r < proxyRows; r++) {
                    // Items in this row (last row may have fewer)
                    const startIdx = r * proxiesPerRow;
                    const endIdx = Math.min(startIdx + proxiesPerRow, z.serviceProxies.length);
                    const itemsInRow = endIdx - startIdx;

                    // Find max height in this row
                    let maxRowHeight = 0;
                    for (let i = startIdx; i < endIdx; i++) {
                        const sp = z.serviceProxies[i];
                        const metrics = z.serviceProxyMetrics[i] || computeServiceProxyMetrics(z.id, sp.destZone);
                        const boxHeight = metrics.expandedHeight || metrics.height;
                        maxRowHeight = Math.max(maxRowHeight, boxHeight);
                    }
                    serviceProxyRowHeights.push(maxRowHeight);
                    serviceProxiesHeight += maxRowHeight;
                    if (r < proxyRows - 1) {
                        serviceProxiesHeight += proxySpacing;
                    }
                }

                let rowStartY = currentY;
                z.serviceProxies.forEach((sp, i) => {
                    const row = Math.floor(i / proxiesPerRow);
                    const col = i % proxiesPerRow;

                    // Calculate actual row width based on items in this row
                    const startIdx = row * proxiesPerRow;
                    const endIdx = Math.min(startIdx + proxiesPerRow, z.serviceProxies.length);
                    const itemsInRow = endIdx - startIdx;
                    const rowWidth = itemsInRow * (z.serviceProxyBoxWidth + proxySpacing) - proxySpacing;
                    const rowUseWidth = Math.min(rowWidth, availableWidth);

                    const spx = leftBoxX + (leftBoxWidth - rowUseWidth) / 2 + col * (z.serviceProxyBoxWidth + proxySpacing) + z.serviceProxyBoxWidth / 2;
                    // Use actual row height for positioning, accumulating from previous rows
                    let spy = rowStartY;
                    for (let r = 0; r < row; r++) {
                        spy += serviceProxyRowHeights[r] + proxySpacing;
                    }
                    spy += serviceProxyRowHeights[row] / 2; // Center in the row

                    const metrics = z.serviceProxyMetrics[i] || computeServiceProxyMetrics(z.id, sp.destZone);
                    let boxWidth = metrics.expandedWidth || metrics.width;
                    let boxHeight = metrics.expandedHeight || metrics.height;
                    const lines = metrics.lines;

                    // Count object proxies that belong to this service proxy and calculate expanded height and width
                    let objectProxiesForThisService = [];
                    if (showObjectProxies && z.objectProxies && z.objectProxies.length > 0) {
                        objectProxiesForThisService = z.objectProxies.filter(op => op.destZone === sp.destZone);
                        if (objectProxiesForThisService.length > 0) {
                            // Expanded sizes already include object proxies in metrics
                        }
                    }

                    const spG = zoneSel.append('g').attr('transform', `translate(${spx},${spy})`);

                    spG.append('rect')
                        .attr('class', 'service-proxy-box')
                        .attr('x', -boxWidth / 2)
                        .attr('y', -boxHeight / 2)
                        .attr('width', boxWidth)
                        .attr('height', boxHeight)
                        .attr('rx', 4);

                    const textStartX = -boxWidth / 2 + serviceProxyBoxPaddingX;
                    const textStartY = -boxHeight / 2 + serviceProxyBoxPaddingY + 9;
                    lines.forEach((line, idx) => {
                        spG.append('text')
                            .attr('class', idx === 0 ? 'service-proxy-label' : 'service-proxy-detail')
                            .attr('x', textStartX)
                            .attr('y', textStartY + idx * serviceProxyLineHeight)
                            .attr('text-anchor', 'start')
                            .text(line);
                    });

                    // Wire from service to service proxy
                    zoneSel.append('line')
                        .attr('class', 'wire service-proxy-link')
                        .attr('data-source-zone', z.id)
                        .attr('data-dest-zone', sp.destZone)
                        .attr('x1', spx)
                        .attr('y1', spy - boxHeight / 2)
                        .attr('x2', serviceX)
                        .attr('y2', serviceY + serviceBoxHeight / 2);

                    // Object proxies inside service proxies
                    if (objectProxiesForThisService.length > 0) {
                        let opCurrentY = spy - boxHeight / 2 + metrics.height + 15; // Match passthrough spacing
                        objectProxiesForThisService.forEach((op, opIdx) => {
                            const opMetrics = computeObjectProxyMetrics(z.id, op.destZone, op.object);
                            const opBoxWidth = opMetrics.width;
                            const opBoxHeight = opMetrics.height;
                            const opLines = opMetrics.lines;

                            const opy = opCurrentY + opBoxHeight / 2;

                            const opG = zoneSel.append('g').attr('transform', `translate(${spx},${opy})`);

                            opG.append('rect')
                                .attr('class', 'object-proxy-box')
                                .attr('data-object', op.object)
                                .attr('data-source-zone', z.id)
                                .attr('data-dest-zone', op.destZone)
                                .attr('x', -opBoxWidth / 2)
                                .attr('y', -opBoxHeight / 2)
                                .attr('width', opBoxWidth)
                                .attr('height', opBoxHeight)
                                .attr('rx', 4);

                            const opTextStartX = -opBoxWidth / 2 + serviceProxyBoxPaddingX;
                            const opTextStartY = -opBoxHeight / 2 + serviceProxyBoxPaddingY + 9;
                            opLines.forEach((line, idx) => {
                                opG.append('text')
                                    .attr('class', idx === 0 ? 'object-proxy-label' : 'object-proxy-detail')
                                    .attr('x', opTextStartX)
                                    .attr('y', opTextStartY + idx * serviceProxyLineHeight)
                                    .attr('text-anchor', 'start')
                                    .text(line);
                            });

                            // Wire from object proxy to service proxy (connect to top edge of service proxy box)
                            zoneSel.append('line')
                                .attr('class', 'wire object-to-service-proxy object-proxy-link')
                                .attr('data-object', op.object)
                                .attr('data-source-zone', z.id)
                                .attr('data-dest-zone', op.destZone)
                                .attr('x1', spx)
                                .attr('y1', opy - opBoxHeight / 2)
                                .attr('x2', spx)
                                .attr('y2', spy - boxHeight / 2 + metrics.height);

                            // Move Y position down for next object proxy
                            opCurrentY += opBoxHeight + 15; // Match passthrough spacing
                        });
                    }
                });
            }

            // === RIGHT BOX CONTENT ===
            // Passthroughs in right box (flowing like text to fill width)
            if (showPassthroughs && z.passthroughs && z.passthroughs.length > 0) {
                const passthroughSpacing = 15;
                const passthroughHSpacing = 15; // Horizontal spacing between passthroughs

                // Calculate how many passthroughs can fit per row based on box widths
                // Use a grid layout that wraps to fill available width
                const maxPassthroughWidth = Math.max(...z.passthroughs.map((p, i) => {
                    const metrics = z.passthroughMetrics[i] || computePassthroughMetrics(p.fwd, p.rev, 0, 0);
                    return metrics.width;
                }), passthroughMinWidth);

                // Calculate passthroughs per row to fill the width
                const passthroughsPerRow = Math.max(1, Math.floor((rightBoxWidth + passthroughHSpacing) / (maxPassthroughWidth + passthroughHSpacing)));
                const passthroughRows = Math.ceil(z.passthroughs.length / passthroughsPerRow);

                // Calculate actual row heights (accounting for variable box heights)
                let rowHeights = new Array(passthroughRows).fill(0);
                z.passthroughs.forEach((p, i) => {
                    const row = Math.floor(i / passthroughsPerRow);
                    const metrics = z.passthroughMetrics[i] || computePassthroughMetrics(p.fwd, p.rev, 0, 0);
                    rowHeights[row] = Math.max(rowHeights[row], metrics.height);
                });

                // Calculate total height
                const totalPassthroughsHeight = rowHeights.reduce((sum, h) => sum + h, 0) + (passthroughRows - 1) * passthroughSpacing;

                // Center passthroughs vertically in the right box
                const passthroughStartY = rightBoxY + (layoutBoxHeight - totalPassthroughsHeight) / 2;

                // Store cumulative row start positions
                let rowStartYs = [passthroughStartY];
                for (let r = 1; r < passthroughRows; r++) {
                    rowStartYs[r] = rowStartYs[r - 1] + rowHeights[r - 1] + passthroughSpacing;
                }

                z.passthroughs.forEach((p, i) => {
                    const row = Math.floor(i / passthroughsPerRow);
                    const col = i % passthroughsPerRow;
                    const metrics = z.passthroughMetrics[i] || computePassthroughMetrics(p.fwd, p.rev, 0, 0);
                    const boxWidth = metrics.width;
                    const boxHeight = metrics.height;
                    const lines = metrics.lines;

                    // Calculate row width for this specific row (may have fewer items in last row)
                    const itemsInRow = Math.min(passthroughsPerRow, z.passthroughs.length - row * passthroughsPerRow);
                    const rowWidth = itemsInRow * (maxPassthroughWidth + passthroughHSpacing) - passthroughHSpacing;

                    // Position: distribute evenly across the row
                    const px = rightBoxX + (rightBoxWidth - rowWidth) / 2 + col * (maxPassthroughWidth + passthroughHSpacing) + maxPassthroughWidth / 2;
                    const py = rowStartYs[row] + rowHeights[row] / 2;

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
                        const routeKey = `${z.id}:${p.fwd}:${p.rev}`;
                        zoneSel.append('line')
                            .attr('class', 'wire routing')
                            .attr('data-zone', z.id)
                            .attr('data-route-key', routeKey)
                            .attr('x1', rP.relX)
                            .attr('y1', rP.relY + (rP.relY === 0 ? -15 : 15))
                            .attr('x2', px)
                            .attr('y2', py + halfBoxHeight);

                        zoneSel.append('line')
                            .attr('class', 'wire routing')
                            .attr('data-zone', z.id)
                            .attr('data-route-key', routeKey)
                            .attr('x1', px)
                            .attr('y1', py - halfBoxHeight)
                            .attr('x2', fP.relX)
                            .attr('y2', fP.relY + (fP.relY === 0 ? -15 : 15));
                    }

                });
            }

            // Render ports and wires (transports)
            if (showTransports) {
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
                        .attr('class', 'wire transport-link')
                        .attr('data-source-zone', zId)
                        .attr('data-adj-zone', adjId)
                        .attr('x1', p.relX)
                        .attr('y1', p.relY + (p.relY === 0 ? -halfBoxHeight : halfBoxHeight))
                        .attr('x2', serviceX)
                        .attr('y2', serviceY + (p.relY === 0 ? 15 : -15));
                });
            }
        });

        // Ensure wires render behind boxes within each zone group.
        nodeGroups.selectAll('.wire').lower();

        bindFlashHandlers();
        refreshFlashHandlers();
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

    function findZonePath(startZone, targetZone) {
        if (startZone === targetZone) {
            return [startZone];
        }
        const queue = [startZone];
        const visited = new Set([startZone]);
        const parent = new Map();

        while (queue.length > 0) {
            const current = queue.shift();
            const neighbors = adjacencyList[current];
            if (!neighbors) {
                continue;
            }
            for (const next of neighbors) {
                if (visited.has(next)) {
                    continue;
                }
                visited.add(next);
                parent.set(next, current);
                if (next === targetZone) {
                    queue.length = 0;
                    break;
                }
                queue.push(next);
            }
        }

        if (!parent.has(targetZone)) {
            return null;
        }
        const path = [targetZone];
        let cursor = targetZone;
        while (parent.has(cursor)) {
            cursor = parent.get(cursor);
            path.push(cursor);
            if (cursor === startZone) {
                break;
            }
        }
        return path.reverse();
    }

    function clearFlashHighlights() {
        d3.selectAll('.flash-line').classed('flash-line', false);
        d3.selectAll('.flash-box').classed('flash-box', false);
    }

    function getRouteKeysForPath(path, endpointA, endpointB) {
        if (!path || path.length < 3) {
            return new Set();
        }
        const routeKeys = new Set();
        const matchA = endpointA;
        const matchB = endpointB;
        for (let i = 1; i < path.length - 1; i++) {
            const zoneId = path[i];
            const zone = zones[zoneId];
            if (!zone || !zone.passthroughs) {
                continue;
            }
            zone.passthroughs.forEach((p) => {
                const forwardMatch = p.fwd === matchA && p.rev === matchB;
                const reverseMatch = p.fwd === matchB && p.rev === matchA;
                if (forwardMatch || reverseMatch) {
                    routeKeys.add(`${zoneId}:${p.fwd}:${p.rev}`);
                }
            });
        }
        return routeKeys;
    }

    function addFlashForPath(path, routeKeys, endpointA, endpointB) {
        if (!path || path.length === 0) {
            return;
        }
        const zoneSet = new Set(path);
        const edgeSet = new Set();
        for (let i = 0; i < path.length - 1; i++) {
            const a = path[i];
            const b = path[i + 1];
            edgeSet.add(`${a}${flashEdgeSeparator}${b}`);
            edgeSet.add(`${b}${flashEdgeSeparator}${a}`);
        }

        d3.selectAll('.trunk-line')
            .filter(function () {
                const source = Number(this.getAttribute('data-source-zone'));
                const target = Number(this.getAttribute('data-target-zone'));
                if (!Number.isFinite(source) || !Number.isFinite(target)) {
                    return false;
                }
                return edgeSet.has(`${source}${flashEdgeSeparator}${target}`);
            })
            .classed('flash-line', true);

        d3.selectAll('.wire.routing')
            .filter(function () {
                const zone = Number(this.getAttribute('data-zone'));
                if (!Number.isFinite(zone) || !zoneSet.has(zone)) {
                    return false;
                }
                if (!routeKeys || routeKeys.size === 0) {
                    return false;
                }
                const key = this.getAttribute('data-route-key');
                return key && routeKeys.has(key);
            })
            .classed('flash-line', true);

        d3.selectAll('.transport-link')
            .filter(function () {
                const source = Number(this.getAttribute('data-source-zone'));
                const adj = Number(this.getAttribute('data-adj-zone'));
                if (!Number.isFinite(source) || !Number.isFinite(adj)) {
                    return false;
                }
                if (source !== endpointA && source !== endpointB) {
                    return false;
                }
                return edgeSet.has(`${source}${flashEdgeSeparator}${adj}`);
            })
            .classed('flash-line', true);
    }

    function flashObjectProxy(objectId, sourceZone, destZone) {
        clearFlashHighlights();
        const path = findZonePath(sourceZone, destZone);
        const routeKeys = getRouteKeysForPath(path, sourceZone, destZone);
        addFlashForPath(path, routeKeys, sourceZone, destZone);

        d3.selectAll('.object-proxy-box')
            .filter(function () {
                return this.getAttribute('data-object') == objectId
                    && this.getAttribute('data-source-zone') == sourceZone
                    && this.getAttribute('data-dest-zone') == destZone;
            })
            .classed('flash-box', true);

        d3.selectAll('.stub-box')
            .filter(function () {
                return this.getAttribute('data-object') == objectId
                    && this.getAttribute('data-zone') == destZone;
            })
            .classed('flash-box', true);

        d3.selectAll('.object-proxy-link')
            .filter(function () {
                return this.getAttribute('data-object') == objectId
                    && this.getAttribute('data-source-zone') == sourceZone
                    && this.getAttribute('data-dest-zone') == destZone;
            })
            .classed('flash-line', true);

        d3.selectAll('.service-proxy-link')
            .filter(function () {
                return this.getAttribute('data-source-zone') == sourceZone
                    && this.getAttribute('data-dest-zone') == destZone;
            })
            .classed('flash-line', true);

        d3.selectAll('.stub-link')
            .filter(function () {
                return this.getAttribute('data-object') == objectId
                    && this.getAttribute('data-zone') == destZone;
            })
            .classed('flash-line', true);
    }

    function flashStub(objectId, zoneId) {
        clearFlashHighlights();
        d3.selectAll('.stub-box')
            .filter(function () {
                return this.getAttribute('data-object') == objectId
                    && this.getAttribute('data-zone') == zoneId;
            })
            .classed('flash-box', true);

        const proxyRects = d3.selectAll('.object-proxy-box')
            .filter(function () {
                return this.getAttribute('data-object') == objectId
                    && this.getAttribute('data-dest-zone') == zoneId;
            });

        const sourceZones = new Set();
        proxyRects.each(function () {
            const sourceZone = Number(this.getAttribute('data-source-zone'));
            if (Number.isFinite(sourceZone)) {
                sourceZones.add(sourceZone);
            }
        });

        proxyRects.classed('flash-box', true);

        d3.selectAll('.object-proxy-link')
            .filter(function () {
                return this.getAttribute('data-object') == objectId
                    && this.getAttribute('data-dest-zone') == zoneId;
            })
            .classed('flash-line', true);

        d3.selectAll('.service-proxy-link')
            .filter(function () {
                return this.getAttribute('data-dest-zone') == zoneId
                    && sourceZones.has(Number(this.getAttribute('data-source-zone')));
            })
            .classed('flash-line', true);

        d3.selectAll('.stub-link')
            .filter(function () {
                return this.getAttribute('data-object') == objectId
                    && this.getAttribute('data-zone') == zoneId;
            })
            .classed('flash-line', true);

        sourceZones.forEach((sourceZone) => {
            const path = findZonePath(sourceZone, zoneId);
            const routeKeys = getRouteKeysForPath(path, sourceZone, zoneId);
            addFlashForPath(path, routeKeys, sourceZone, zoneId);
        });
    }

    function bindFlashHandlers() {
        if (flashHandlersBound) {
            return;
        }
        flashHandlersBound = true;

        const svgSelection = d3.select('svg');
        if (!svgSelection.empty()) {
            svgSelection.on('pointerup.flash', clearFlashHighlights);
            svgSelection.on('pointerleave.flash', clearFlashHighlights);
            svgSelection.on('pointercancel.flash', clearFlashHighlights);
        }

        d3.select(window).on('pointerup.flash', clearFlashHighlights);
    }

    function refreshFlashHandlers() {
        d3.selectAll('rect.object-proxy-box').on('pointerdown.flash', function (event) {
            event.stopPropagation();
            const objectId = this.getAttribute('data-object');
            const sourceZone = Number(this.getAttribute('data-source-zone'));
            const destZone = Number(this.getAttribute('data-dest-zone'));
            if (objectId !== null && Number.isFinite(sourceZone) && Number.isFinite(destZone)) {
                flashObjectProxy(objectId, sourceZone, destZone);
            }
        });

        d3.selectAll('rect.stub-box').on('pointerdown.flash', function (event) {
            event.stopPropagation();
            const objectId = this.getAttribute('data-object');
            const zoneId = Number(this.getAttribute('data-zone'));
            if (objectId !== null && Number.isFinite(zoneId)) {
                flashStub(objectId, zoneId);
            }
        });
    }

}

window.initAnimationTelemetry = initAnimationTelemetry;
