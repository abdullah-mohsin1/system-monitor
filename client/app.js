'use strict';

const SYSTEM_POLL_MS = 1000;
const PROCESS_POLL_MS = 2500;
const REQUEST_TIMEOUT_MS = 4000;
const HISTORY_SAMPLES = 60;

const WARN_PERCENT = 75;
const CRIT_PERCENT = 90;

const elements = {
    status: document.getElementById('status'),
    statusText: document.getElementById('statusText'),
    metaCores: document.getElementById('metaCores'),
    metaUptime: document.getElementById('metaUptime'),
    metaUpdated: document.getElementById('metaUpdated'),
    processBody: document.getElementById('processBody'),
    processSearch: document.getElementById('processSearch'),
    charts: document.getElementById('charts'),
};

// The most recent process list is kept so filtering does not need a new request.
let latestProcesses = [];

// History lives only in memory: reloading the page starts the graphs over.
const charts = { cpu: null, memory: null };

/* ---------- formatting ---------- */

const BYTE_UNITS = ['B', 'KB', 'MB', 'GB', 'TB', 'PB'];

function formatBytes(bytes) {
    if (!Number.isFinite(bytes) || bytes < 0) {
        return '—';
    }

    let value = bytes;
    let unit = 0;
    while (value >= 1024 && unit < BYTE_UNITS.length - 1) {
        value /= 1024;
        unit += 1;
    }

    // Whole numbers read better once the value is large; small ones need a decimal.
    const decimals = unit === 0 || value >= 100 ? 0 : 1;
    return `${value.toFixed(decimals)} ${BYTE_UNITS[unit]}`;
}

function formatUptime(totalSeconds) {
    if (!Number.isFinite(totalSeconds) || totalSeconds < 0) {
        return '—';
    }

    const days = Math.floor(totalSeconds / 86400);
    const hours = Math.floor((totalSeconds % 86400) / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);

    const parts = [];
    if (days > 0) parts.push(`${days}d`);
    if (days > 0 || hours > 0) parts.push(`${hours}h`);
    parts.push(`${minutes}m`);
    return parts.join(' ');
}

function formatPercent(percent) {
    return Number.isFinite(percent) ? `${percent.toFixed(1)}%` : '—';
}

function formatClockTime(date) {
    return date.toLocaleTimeString([], { hour12: false });
}

/* ---------- rendering ---------- */

function levelFor(percent) {
    if (percent >= CRIT_PERCENT) return 'crit';
    if (percent >= WARN_PERCENT) return 'warn';
    return 'ok';
}

function updateCard(prefix, percent, detail) {
    const value = document.getElementById(`${prefix}Value`);
    const detailNode = document.getElementById(`${prefix}Detail`);
    const bar = document.getElementById(`${prefix}Bar`);

    value.textContent = formatPercent(percent);
    detailNode.textContent = detail;
    bar.style.width = `${Math.min(Math.max(percent, 0), 100)}%`;
    bar.dataset.level = levelFor(percent);
}

function renderSystem(system) {
    if (system.cpu) {
        updateCard('cpu', system.cpu.usagePercent, `${system.cpu.coreCount} cores`);
        elements.metaCores.textContent = system.cpu.coreCount;
    }

    if (system.memory) {
        const { usedBytes, totalBytes, usagePercent } = system.memory;
        updateCard('memory', usagePercent, `${formatBytes(usedBytes)} / ${formatBytes(totalBytes)}`);
    }

    if (system.disk) {
        const { usedBytes, totalBytes, usagePercent } = system.disk;
        updateCard('disk', usagePercent, `${formatBytes(usedBytes)} / ${formatBytes(totalBytes)}`);
    }

    if (system.uptimeSeconds !== null && system.uptimeSeconds !== undefined) {
        elements.metaUptime.textContent = formatUptime(system.uptimeSeconds);
    }

    const now = new Date();
    const timeLabel = formatClockTime(now);
    if (system.cpu) {
        pushSample(charts.cpu, timeLabel, system.cpu.usagePercent);
    }
    if (system.memory) {
        pushSample(charts.memory, timeLabel, system.memory.usagePercent);
    }
    elements.metaUpdated.textContent = timeLabel;
}

function renderProcesses(processes) {
    const query = elements.processSearch.value.trim().toLowerCase();
    const visible = query
        ? processes.filter(
              (process) =>
                  process.name.toLowerCase().includes(query) || String(process.pid).includes(query),
          )
        : processes;

    const body = document.createDocumentFragment();

    if (visible.length === 0) {
        const row = document.createElement('tr');
        row.className = 'empty';
        const cell = document.createElement('td');
        cell.colSpan = 4;
        cell.textContent = query ? `No processes match “${query}”` : 'No processes reported';
        row.append(cell);
        body.append(row);
    }

    for (const process of visible) {
        const row = document.createElement('tr');
        // Process names come straight from /proc, so they are only ever written
        // as text, never as markup.
        row.append(
            cell(String(process.pid), 'num'),
            cell(process.name, 'name'),
            cell(formatPercent(process.cpuPercent), 'num'),
            cell(formatBytes(process.memoryBytes), 'num'),
        );
        body.append(row);
    }

    elements.processBody.replaceChildren(body);
}

function cell(text, className) {
    const td = document.createElement('td');
    td.textContent = text;
    if (className) td.className = className;
    return td;
}

function setConnectionState(state, message) {
    elements.status.dataset.state = state;
    elements.statusText.textContent = message;
}

/* ---------- history charts ---------- */

const CHART_COLOURS = {
    cpu: '#3987e5',
    memory: '#d55181',
};

// Draws the vertical hover line that Chart.js does not provide itself.
const crosshairPlugin = {
    id: 'crosshair',
    afterDatasetsDraw(chart) {
        const active = chart.getActiveElements();
        if (active.length === 0) {
            return;
        }

        const { ctx, chartArea } = chart;
        ctx.save();
        ctx.strokeStyle = 'rgba(230, 237, 243, 0.28)';
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(active[0].element.x, chartArea.top);
        ctx.lineTo(active[0].element.x, chartArea.bottom);
        ctx.stroke();
        ctx.restore();
    },
};

function createHistoryChart(canvasId, label, colour) {
    return new Chart(document.getElementById(canvasId), {
        type: 'line',
        data: {
            labels: [],
            datasets: [
                {
                    label,
                    data: [],
                    borderColor: colour,
                    backgroundColor: `${colour}22`,
                    borderWidth: 2,
                    fill: true,
                    tension: 0.25,
                    pointRadius: 0,
                    pointHoverRadius: 4,
                    pointHoverBackgroundColor: colour,
                    pointHoverBorderColor: '#161b22',
                    pointHoverBorderWidth: 2,
                },
            ],
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            animation: false,  // live data; animating every second only adds jitter
            interaction: { mode: 'index', intersect: false },
            plugins: {
                legend: { display: false },  // one series, named by the panel heading
                tooltip: {
                    displayColors: false,
                    callbacks: {
                        label: (context) => `${label}: ${context.parsed.y.toFixed(1)}%`,
                    },
                },
            },
            scales: {
                x: {
                    grid: { display: false },
                    ticks: {
                        color: '#8b949e',
                        maxTicksLimit: 5,
                        maxRotation: 0,
                        autoSkip: true,
                    },
                },
                y: {
                    min: 0,
                    max: 100,
                    grid: { color: 'rgba(39, 46, 56, 0.9)' },
                    border: { display: false },
                    ticks: {
                        color: '#8b949e',
                        stepSize: 25,
                        callback: (value) => `${value}%`,
                    },
                },
            },
        },
        plugins: [crosshairPlugin],
    });
}

function initCharts() {
    if (typeof Chart === 'undefined') {
        elements.charts.remove();  // CDN unreachable: drop the section, keep the dashboard
        return;
    }
    charts.cpu = createHistoryChart('cpuChart', 'CPU', CHART_COLOURS.cpu);
    charts.memory = createHistoryChart('memoryChart', 'Memory', CHART_COLOURS.memory);
}

function pushSample(chart, timeLabel, percent) {
    if (!chart || !Number.isFinite(percent)) {
        return;
    }

    chart.data.labels.push(timeLabel);
    chart.data.datasets[0].data.push(percent);
    if (chart.data.labels.length > HISTORY_SAMPLES) {
        chart.data.labels.shift();
        chart.data.datasets[0].data.shift();
    }
    chart.update('none');
}

/* ---------- polling ---------- */

async function fetchJson(path) {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
    try {
        const response = await fetch(path, { signal: controller.signal, cache: 'no-store' });
        if (!response.ok) {
            throw new Error(`${path} returned ${response.status}`);
        }
        return await response.json();
    } finally {
        clearTimeout(timeout);
    }
}

async function pollSystem() {
    try {
        renderSystem(await fetchJson('/api/system'));
        setConnectionState('live', 'Live');
    } catch (error) {
        // The last known values stay on screen; only the badge changes, so a
        // single failed request does not blank the dashboard.
        setConnectionState('error', 'Disconnected');
        console.warn('system poll failed:', error.message);
    }
}

async function pollProcesses() {
    try {
        latestProcesses = await fetchJson('/api/processes');
        renderProcesses(latestProcesses);
    } catch (error) {
        console.warn('process poll failed:', error.message);
    }
}

elements.processSearch.addEventListener('input', () => renderProcesses(latestProcesses));

initCharts();
pollSystem();
pollProcesses();
setInterval(pollSystem, SYSTEM_POLL_MS);
setInterval(pollProcesses, PROCESS_POLL_MS);
