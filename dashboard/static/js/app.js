/*
   static/js/app.js
   Logic Frontend Dashboard Monitoring Rehabilitasi Sendi Siku 1-DOF.
*/

// --- State Aplikasi ---
let socket = null;
let isConnectedToSerial = false;
let serialPort = null;
let serialBaud = 115200;
let isConnectedToWifi = false;
let wifiClientIp = null;
let connectionMode = "usb"; // "usb" atau "wifi"

// Data untuk Chart
let chartAngles = null;
let chartFuzzyInputs = null;
let chartFuzzyOutput = null;

const maxChartDataPoints = 60; // window 30 detik (jika data 500ms sekali)
let chartTimeLabels = [];
let targetData = [];
let actualData = [];
let errorData = [];
let deltaErrorData = [];
let fuzzyOutData = [];
let chartPaused = false;
let startTime = null;

// Data sesi untuk chart per sesi (Chart 2 & Chart 3)
let sessionChartLabels = [];
let sessionPreTestScores = [];
let sessionPostTestScores = [];
let sessionChartScores = [];

// Data Riwayat Sesi (Tabel)
let sessionData = [];


// ==========================================================================
// INISIALISASI
// ==========================================================================
document.addEventListener("DOMContentLoaded", () => {
    initClock();
    initWebSocket();
    initCharts();
    initEventHandlers();
    refreshPortsList();
});

// --- Jam Real-Time ---
function initClock() {
    const clockTime = document.getElementById("clock-time");
    const clockDate = document.getElementById("clock-date");
    
    function updateClock() {
        const now = new Date();
        const hrs = String(now.getHours()).padStart(2, '0');
        const mins = String(now.getMinutes()).padStart(2, '0');
        const secs = String(now.getSeconds()).padStart(2, '0');
        clockTime.textContent = `${hrs}:${mins}:${secs}`;
        
        const day = String(now.getDate()).padStart(2, '0');
        const month = String(now.getMonth() + 1).padStart(2, '0');
        const year = now.getFullYear();
        clockDate.textContent = `${day}/${month}/${year}`;
    }
    
    updateClock();
    setInterval(updateClock, 1000);
}


// ==========================================================================
// WEBSOCKET & HTTP API
// ==========================================================================
function initWebSocket() {
    const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
    const wsUrl = `${protocol}//${window.location.host}/ws`;
    
    socket = new WebSocket(wsUrl);
    
    socket.onopen = () => {
        console.log("WebSocket connected.");
    };
    
    socket.onmessage = (event) => {
        const msg = JSON.parse(event.data);
        handleSocketMessage(msg);
    };
    
    socket.onclose = () => {
        console.log("WebSocket disconnected. Retrying in 3 seconds...");
        updateConnectionBadge(false);
        setTimeout(initWebSocket, 3000);
    };
}

function handleSocketMessage(msg) {
    switch (msg.type) {
        case "status":
            isConnectedToSerial = msg.connected;
            serialPort = msg.port;
            serialBaud = msg.baud_rate;
            isConnectedToWifi = msg.wifi_connected;
            
            // Tampilkan IP Target untuk ESP32
            if (msg.local_ip && msg.tcp_port) {
                document.getElementById("wifi-target-ip").textContent = `${msg.local_ip}:${msg.tcp_port}`;
            }
            
            updateConnectionBadge();
            break;
            
        case "wifi_status":
            isConnectedToWifi = msg.connected;
            wifiClientIp = msg.client_ip;
            updateConnectionBadge();
            break;
            
        case "realtime":
            updateRealtimeCards(msg.data);
            updateSafetyStatus(msg.data);
            appendDataToCharts(msg.data);
            break;
            
        case "csv_status":
            handleCsvStatus(msg);
            break;
            
        case "csv_data":
            sessionData = msg.data;
            populateHistoryTable(sessionData);
            break;
            
        case "error":
            alert(`Serial Error: ${msg.message}`);
            break;
    }
}

// Refresh Daftar Port COM dari Backend
async function refreshPortsList() {
    const portSelect = document.getElementById("port-select");
    portSelect.innerHTML = '<option value="">Memindai...</option>';
    
    try {
        const res = await fetch("/api/ports");
        const data = await res.json();
        portSelect.innerHTML = "";
        
        if (data.ports && data.ports.length > 0) {
            data.ports.forEach(port => {
                const opt = document.createElement("option");
                opt.value = port;
                opt.textContent = port;
                portSelect.appendChild(opt);
            });
        } else {
            portSelect.innerHTML = '<option value="">(tidak ada port)</option>';
        }
    } catch (err) {
        console.error("Gagal scan port:", err);
        portSelect.innerHTML = '<option value="">Gagal scan</option>';
    }
}


// ==========================================================================
// TAMPILAN: CONNECTION BADGE
// ==========================================================================
function updateConnectionBadge() {
    const badge = document.getElementById("conn-status");
    const badgeText = badge.querySelector(".status-text");
    const btnConnect = document.getElementById("btn-connect");
    
    if (connectionMode === "usb") {
        if (isConnectedToSerial) {
            badge.className = "status-badge connected";
            badgeText.textContent = `USB: TERHUBUNG ${serialPort} @ ${serialBaud}`;
            btnConnect.textContent = "Putuskan";
            btnConnect.className = "btn-primary connected";
        } else {
            badge.className = "status-badge disconnected";
            badgeText.textContent = "USB: TIDAK TERHUBUNG";
            btnConnect.textContent = "Hubungkan";
            btnConnect.className = "btn-primary";
            resetDashboardValues();
        }
    } else { // wifi mode
        if (isConnectedToWifi) {
            badge.className = "status-badge connected";
            badgeText.textContent = `WIFI: ESP32 TERKONEKSI (${wifiClientIp})`;
        } else {
            badge.className = "status-badge disconnected";
            badgeText.textContent = "WIFI: MENUNGGU ESP32...";
            resetDashboardValues();
        }
    }
}

function resetDashboardValues() {
    document.getElementById("val-target").textContent = "---";
    document.getElementById("val-actual").textContent = "---";
    document.getElementById("val-error").textContent = "---";
    document.getElementById("val-fuzzy").textContent = "---";
    
    document.getElementById("val-sensor-err").textContent = "---";
    document.getElementById("val-sensor-err").style.color = "var(--color-sensor-err)";
    
    document.getElementById("bar-actual").style.width = "0%";
    document.getElementById("bar-fuzzy").style.width = "0%";
    
    // Reset Safety
    document.getElementById("ind-motor").className = "safety-indicator";
    document.getElementById("ind-motor").querySelector(".text").textContent = "---";
    document.getElementById("ind-limit-atas").className = "safety-indicator";
    document.getElementById("ind-limit-atas").querySelector(".text").textContent = "---";
    document.getElementById("ind-limit-bawah").className = "safety-indicator";
    document.getElementById("ind-limit-bawah").querySelector(".text").textContent = "---";
    document.getElementById("ind-sensor").className = "safety-indicator";
    document.getElementById("ind-sensor").querySelector(".text").textContent = "---";
    
    const badgeMode = document.getElementById("badge-mode");
    badgeMode.textContent = "---";
    badgeMode.className = "safety-badge";
    badgeMode.style.backgroundColor = "transparent";
    badgeMode.style.color = "var(--text-secondary)";
    
    document.getElementById("safety-card").className = "safety-panel glassmorphic";
}


// ==========================================================================
// FITUR 02: UPDATE REAL-TIME CARDS
// ==========================================================================
function updateRealtimeCards(data) {
    const target = data.sudut_target !== undefined ? data.sudut_target : 0;
    const actual = data.sudut_aktual !== undefined ? data.sudut_aktual : 0;
    const error  = data.error !== undefined ? data.error : 0;
    const fuzzy  = data.output_fuzzy !== undefined ? data.output_fuzzy : 0;
    
    // 1. Target
    document.getElementById("val-target").textContent = `${target.toFixed(1)}°`;
    
    // 2. Aktual
    const valActual = document.getElementById("val-actual");
    valActual.textContent = `${actual.toFixed(1)}°`;
    
    const barActual = document.getElementById("bar-actual");
    const pctActual = (actual / 180) * 100;
    barActual.style.width = `${Math.min(pctActual, 100)}%`;
    
    // Color coding aktual berdasarkan error
    const absE = Math.abs(error);
    let colorHex = "var(--color-safety-ok)";
    if (absE > 15) {
        colorHex = "var(--color-safety-err)";
    } else if (absE > 5) {
        colorHex = "var(--color-safety-warn)";
    }
    valActual.style.color = colorHex;
    barActual.style.backgroundColor = colorHex;
    
    // 3. Error
    const valError = document.getElementById("val-error");
    valError.textContent = `${error >= 0 ? '+' : ''}${error.toFixed(1)}°`;
    valError.style.color = colorHex;
    
    // 4. Output Fuzzy
    document.getElementById("val-fuzzy").textContent = `${fuzzy.toFixed(1)}`;
    const barFuzzy = document.getElementById("bar-fuzzy");
    barFuzzy.style.width = `${Math.min(fuzzy, 100)}%`;

    // 5. Error Sensor — pakai nilai yang sama dengan card ERROR (data.error)
    const valSensorErr = document.getElementById("val-sensor-err");
    valSensorErr.textContent = `${error >= 0 ? '+' : ''}${error.toFixed(1)}°`;
    valSensorErr.style.color = colorHex;
}


// ==========================================================================
// FITUR 03: SAFETY STATUS PANEL
// ==========================================================================
function updateSafetyStatus(data) {
    // 1. Motor
    const indMotor = document.getElementById("ind-motor");
    const motorOn = data.motor_on;
    indMotor.className = `safety-indicator ${motorOn ? 'ok pulse-animation' : 'err'}`;
    indMotor.querySelector(".text").textContent = motorOn ? "ON" : "OFF";
    
    // 2. Limit Atas
    const indLimitAtas = document.getElementById("ind-limit-atas");
    const limitAtas = data.limit_atas;
    indLimitAtas.className = `safety-indicator ${limitAtas ? 'err' : 'ok'}`;
    indLimitAtas.querySelector(".text").textContent = limitAtas ? "AKTIF ⚠" : "NORMAL";
    
    // 3. Limit Bawah
    const indLimitBawah = document.getElementById("ind-limit-bawah");
    const limitBawah = data.limit_bawah;
    indLimitBawah.className = `safety-indicator ${limitBawah ? 'err' : 'ok'}`;
    indLimitBawah.querySelector(".text").textContent = limitBawah ? "AKTIF ⚠" : "NORMAL";
    
    // 4. Mode
    const badgeMode = document.getElementById("badge-mode");
    const mode = (data.mode || "IDLE").toUpperCase();
    badgeMode.textContent = mode;
    
    let modeBg = "var(--color-idle)";
    let modeColor = "#ffffff";
    if (mode === "FLEKSI") {
        modeBg = "#42a5f5";
    } else if (mode === "EKSTENSI") {
        modeBg = "#26c6da";
    } else if (mode === "RESET") {
        modeBg = "var(--color-reset)";
        modeColor = "#000000";
    } else if (mode === "ERROR") {
        modeBg = "var(--color-safety-err)";
    }
    badgeMode.style.backgroundColor = modeBg;
    badgeMode.style.color = modeColor;
    
    // 5. Sensor
    const indSensor = document.getElementById("ind-sensor");
    const sensorOk = data.sensor_ok;
    indSensor.className = `safety-indicator ${sensorOk ? 'ok' : 'err pulse-animation'}`;
    indSensor.querySelector(".text").textContent = sensorOk ? "OK" : "ERROR BACA!";
    
    // Critical Alert: berkedip merah jika ada kondisi kritis
    const safetyCard = document.getElementById("safety-card");
    const isCritical = limitAtas || limitBawah || !sensorOk || mode === "ERROR";
    if (isCritical) {
        safetyCard.classList.add("critical-alert");
    } else {
        safetyCard.classList.remove("critical-alert");
    }
}


// ==========================================================================
// FITUR 04: REAL-TIME CHARTS (CHART.JS)
// ==========================================================================
function initCharts() {
    const ctxAngles = document.getElementById("chart-angles").getContext("2d");
    const ctxFuzzyInputs = document.getElementById("chart-fuzzy-inputs").getContext("2d");
    const ctxFuzzyOutput = document.getElementById("chart-fuzzy-output").getContext("2d");
    
    const chartOptions = {
        responsive: true,
        maintainAspectRatio: false,
        animation: { duration: 0 },
        hover: { mode: null },
        plugins: {
            legend: {
                labels: { color: "#8b949e", font: { family: "Inter", size: 10 } }
            }
        },
        scales: {
            x: {
                grid: { color: "rgba(48, 54, 61, 0.2)" },
                ticks: { color: "#8b949e", font: { size: 9 } }
            },
            y: {
                grid: { color: "rgba(48, 54, 61, 0.2)" },
                ticks: { color: "#8b949e", font: { size: 9 } }
            }
        }
    };
    
    // Chart 1: Target vs Aktual (skala 0-100°)
    chartAngles = new Chart(ctxAngles, {
        type: 'line',
        data: {
            labels: chartTimeLabels,
            datasets: [
                {
                    label: 'Sudut Target',
                    data: targetData,
                    borderColor: '#4fc3f7',
                    borderWidth: 2,
                    borderDash: [5, 5],
                    pointRadius: 0,
                    fill: false
                },
                {
                    label: 'Sudut Aktual',
                    data: actualData,
                    borderColor: '#66bb6a',
                    borderWidth: 2.5,
                    pointRadius: 0,
                    fill: false
                }
            ]
        },
        options: {
            ...chartOptions,
            scales: {
                ...chartOptions.scales,
                y: { ...chartOptions.scales.y, min: 0, max: 100 }
            }
        }
    });
    
    // Chart 2: Pembanding Median Pre-Test & Post-Test per Sesi (dari data CSV)
    chartFuzzyInputs = new Chart(ctxFuzzyInputs, {
        type: 'line',
        data: {
            labels: sessionChartLabels,
            datasets: [
                {
                    label: 'Median Pre-Test',
                    data: sessionPreTestScores,
                    borderColor: '#ffa726',
                    backgroundColor: 'rgba(255, 167, 38, 0.1)',
                    borderWidth: 2.5,
                    pointRadius: 5,
                    pointHoverRadius: 7,
                    pointBackgroundColor: '#ffa726',
                    fill: false,
                    tension: 0.3
                },
                {
                    label: 'Median Post-Test',
                    data: sessionPostTestScores,
                    borderColor: '#ab47bc',
                    backgroundColor: 'rgba(171, 71, 188, 0.1)',
                    borderWidth: 2.5,
                    pointRadius: 5,
                    pointHoverRadius: 7,
                    pointBackgroundColor: '#ab47bc',
                    fill: false,
                    tension: 0.3
                }
            ]
        },
        options: {
            ...chartOptions,
            animation: { duration: 400 },
            hover: { mode: 'index', intersect: false },
            plugins: {
                ...chartOptions.plugins,
                legend: {
                    labels: { color: "#8b949e", font: { family: "Inter", size: 10 } }
                },
                tooltip: {
                    callbacks: {
                        label: ctx => ` ${ctx.dataset.label}: ${ctx.parsed.y !== null ? ctx.parsed.y.toFixed(1) + '°' : '-'}`
                    }
                }
            },
            scales: {
                x: {
                    ...chartOptions.scales.x,
                    title: {
                        display: true,
                        text: 'No. Sesi',
                        color: '#8b949e',
                        font: { size: 9 }
                    }
                },
                y: {
                    ...chartOptions.scales.y,
                    min: 0,
                    max: 100,
                    title: {
                        display: true,
                        text: 'Sudut (°)',
                        color: '#8b949e',
                        font: { size: 9 }
                    }
                }
            }
        }
    });
    
    // Chart 3: Skor Fuzzy per Sesi (dari data CSV) - line chart
    chartFuzzyOutput = new Chart(ctxFuzzyOutput, {
        type: 'line',
        data: {
            labels: sessionChartLabels,
            datasets: [
                {
                    label: 'Skor Fuzzy per Sesi',
                    data: sessionChartScores,
                    borderColor: '#26c6da',
                    backgroundColor: 'rgba(38, 198, 218, 0.1)',
                    borderWidth: 2.5,
                    pointRadius: 5,
                    pointHoverRadius: 7,
                    pointBackgroundColor: '#26c6da',
                    fill: true,
                    tension: 0.3
                }
            ]
        },
        options: {
            ...chartOptions,
            animation: { duration: 400 },
            hover: { mode: 'index', intersect: false },
            plugins: {
                ...chartOptions.plugins,
                legend: {
                    labels: { color: "#8b949e", font: { family: "Inter", size: 10 } }
                },
                tooltip: {
                    callbacks: {
                        label: ctx => ` Skor: ${ctx.parsed.y.toFixed(1)}`
                    }
                }
            },
            scales: {
                x: {
                    ...chartOptions.scales.x,
                    title: {
                        display: true,
                        text: 'No. Sesi',
                        color: '#8b949e',
                        font: { size: 9 }
                    }
                },
                y: {
                    ...chartOptions.scales.y,
                    min: 0,
                    max: 100,
                    title: {
                        display: true,
                        text: 'Skor Fuzzy',
                        color: '#8b949e',
                        font: { size: 9 }
                    }
                }
            }
        }
    });
}

function appendDataToCharts(data) {
    if (chartPaused) return;
    
    const ts = data.timestamp;
    if (startTime === null) {
        startTime = ts;
    }
    const tSeconds = ((ts - startTime) / 1000).toFixed(1);
    
    chartTimeLabels.push(`${tSeconds}s`);
    targetData.push(data.sudut_target);
    actualData.push(data.sudut_aktual);
    errorData.push(data.fuzzy_input_error !== undefined ? data.fuzzy_input_error : data.error);
    deltaErrorData.push(data.fuzzy_input_delta_error || 0);
    
    // Limit data points
    if (chartTimeLabels.length > maxChartDataPoints) {
        chartTimeLabels.shift();
        targetData.shift();
        actualData.shift();
        errorData.shift();
        deltaErrorData.shift();
    }
    
    // Update hanya chart realtime utama (Target vs Aktual)
    chartAngles.update();
}

// Update Chart 2 & Chart 3 dari data sesi CSV
function updateSessionCharts(data) {
    sessionChartLabels.length = 0;
    sessionPreTestScores.length = 0;
    sessionPostTestScores.length = 0;
    sessionChartScores.length = 0;

    data.forEach((row, idx) => {
        const no = row.no !== undefined && row.no !== '' ? row.no : (idx + 1);
        
        let preVal = parseFloat(row.median_pretest !== undefined ? row.median_pretest : row.median_pre);
        let postVal = parseFloat(row.median_posttest !== undefined ? row.median_posttest : row.median_post);
        let skorVal = parseFloat(row.skor_fuzzy);

        sessionChartLabels.push(`Sesi ${no}`);
        sessionPreTestScores.push(isNaN(preVal) ? 0 : preVal);
        sessionPostTestScores.push(isNaN(postVal) ? 0 : postVal);
        sessionChartScores.push(isNaN(skorVal) ? 0 : skorVal);
    });

    if (chartFuzzyInputs) chartFuzzyInputs.update();
    if (chartFuzzyOutput) chartFuzzyOutput.update();
}


// ==========================================================================
// FITUR 05: SESSION HISTORY & TABLE
// ==========================================================================

// Populaasi data ke tabel HTML
function populateHistoryTable(data) {
    const tbody = document.getElementById("table-body");
    const countLabel = document.getElementById("txt-total-sessions");
    
    countLabel.textContent = `Total: ${data.length} sesi`;
    
    if (data.length === 0) {
        tbody.innerHTML = '<tr><td colspan="10" class="empty-row">Belum ada data dimuat. Silakan buka CSV atau minta data dari ESP32.</td></tr>';
        updateSessionCharts([]);
        return;
    }
    
    tbody.innerHTML = "";
    
    data.forEach(row => {
        const tr = document.createElement("tr");
        
        // Format values
        const no = row.no !== undefined ? row.no : "-";
        const target = row.sudut_target !== undefined ? parseFloat(row.sudut_target).toFixed(1) : "-";
        const repTarget = row.repetisi_target !== undefined ? row.repetisi_target : "-";
        const repDone = row.repetisi_selesai !== undefined ? row.repetisi_selesai : "-";
        const pretest = row.median_pretest !== undefined ? parseFloat(row.median_pretest).toFixed(1) : "-";
        const posttest = row.median_posttest !== undefined ? parseFloat(row.median_posttest).toFixed(1) : "-";
        const delta = row.delta_perkembangan !== undefined ? parseFloat(row.delta_perkembangan).toFixed(1) : "-";
        const fuzzy = row.skor_fuzzy !== undefined ? parseFloat(row.skor_fuzzy).toFixed(1) : "-";
        
        const category = row.kategori_hasil || "-";
        const status = row.status_sesi || "-";
        
        // Kategori badge class
        let katBadgeClass = "tbl-badge";
        const katClean = category.toLowerCase().replace(/\s+/g, '-');
        if (katClean === "sangat-baik") katBadgeClass += " badge-sangat-baik";
        else if (katClean === "baik") katBadgeClass += " badge-baik";
        else if (katClean === "cukup") katBadgeClass += " badge-cukup";
        else if (katClean === "kurang") katBadgeClass += " badge-kurang";
        else if (katClean === "buruk") katBadgeClass += " badge-buruk";
        
        // Status badge class
        let statBadgeClass = "tbl-badge";
        const statClean = status.toLowerCase();
        if (statClean === "selesai") statBadgeClass += " badge-status-selesai";
        else if (statClean === "terhenti") statBadgeClass += " badge-status-terhenti";
        else if (statClean === "error") statBadgeClass += " badge-status-error";
        
        tr.innerHTML = `
            <td>${no}</td>
            <td>${target}</td>
            <td>${repTarget}</td>
            <td>${repDone}</td>
            <td>${pretest}</td>
            <td>${posttest}</td>
            <td>${delta}</td>
            <td>${fuzzy}</td>
            <td><span class="${katBadgeClass}">${category}</span></td>
            <td><span class="${statBadgeClass}">${status}</span></td>
        `;
        
        tbody.appendChild(tr);
    });

    // Update chart 2 dan chart 3 dengan data sesi penuh (bukan filter)
    updateSessionCharts(sessionData);
}

// Filter/Search Tabel
function filterHistoryTable(query) {
    const q = query.toLowerCase().trim();
    if (!q) {
        populateHistoryTable(sessionData);
        return;
    }
    
    const filtered = sessionData.filter(row => {
        const category = (row.kategori_hasil || "").toLowerCase();
        const status = (row.status_sesi || "").toLowerCase();
        return category.includes(q) || status.includes(q);
    });
    
    populateHistoryTable(filtered);
}

// Client-side local CSV parser (menggunakan FileReader biasa)
function handleLocalCsvSelect(file) {
    const reader = new FileReader();
    reader.onload = function(e) {
        const content = e.target.result;
        parseCsvText(content);
    };
    reader.readAsText(file);
}

function parseCsvText(text) {
    const lines = text.split("\n");
    if (lines.length < 2) return;
    
    // Header
    const headers = lines[0].split(",").map(h => h.trim().toLowerCase());
    const result = [];
    
    for (let i = 1; i < lines.length; i++) {
        const line = lines[i].trim();
        if (!line) continue;
        
        const cols = line.split(",").map(c => c.trim());
        const rowObj = {};
        
        headers.forEach((header, index) => {
            rowObj[header] = cols[index];
        });
        
        result.push(rowObj);
    }
    
    sessionData = result;
    populateHistoryTable(sessionData);
}

// Status loading untuk request CSV ESP32
function handleCsvStatus(msg) {
    const btnRequest = document.getElementById("btn-request-esp");
    if (msg.status === "receiving") {
        btnRequest.textContent = "⏳ Mengunduh...";
        btnRequest.disabled = true;
    } else if (msg.status === "finished") {
        btnRequest.textContent = "📡 Minta Data ESP32";
        btnRequest.disabled = false;
    } else if (msg.status === "error") {
        alert(msg.message);
        btnRequest.textContent = "📡 Minta Data ESP32";
        btnRequest.disabled = false;
    }
}

// Post ke backend untuk ekspor data ke Excel dan unduh file hasil
async function exportToExcel() {
    if (sessionData.length === 0) {
        alert("Tidak ada data untuk diekspor!");
        return;
    }
    
    const confirmation = confirm(`Ekspor ${sessionData.length} baris data ke Excel?`);
    if (!confirmation) return;
    
    try {
        const res = await fetch("/api/export", {
            method: "POST",
            headers: {
                "Content-Type": "application/json"
            },
            body: JSON.stringify({ data: sessionData })
        });
        
        if (!res.ok) throw new Error("Gagal mengunduh file Excel dari server.");
        
        const blob = await res.blob();
        const url = window.URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.style.display = "none";
        a.href = url;
        
        const dateStr = new Date().toISOString().slice(0, 10).replace(/-/g, "");
        a.download = `riwayat_rehabilitasi_${dateStr}.xlsx`;
        document.body.appendChild(a);
        a.click();
        window.URL.revokeObjectURL(url);
        document.body.removeChild(a);
        
    } catch (err) {
        console.error(err);
        alert(`Gagal mengekspor Excel: ${err.message}`);
    }
}


// ==========================================================================
// EVENT HANDLERS
// ==========================================================================
function initEventHandlers() {
    // Mode selector (USB vs WiFi)
    const modeSelect = document.getElementById("mode-select");
    const panelUsb = document.getElementById("panel-usb");
    const panelWifi = document.getElementById("panel-wifi");
    
    modeSelect.addEventListener("change", (e) => {
        connectionMode = e.target.value;
        if (connectionMode === "usb") {
            panelUsb.style.display = "flex";
            panelWifi.style.display = "none";
        } else {
            panelUsb.style.display = "none";
            panelWifi.style.display = "flex";
        }
        updateConnectionBadge();
    });

    // Port controls
    const btnRefreshPorts = document.getElementById("btn-refresh-ports");
    btnRefreshPorts.addEventListener("click", refreshPortsList);
    
    const btnConnect = document.getElementById("btn-connect");
    btnConnect.addEventListener("click", toggleConnection);
    
    // Charts Controls
    const btnChartPause = document.getElementById("btn-chart-pause");
    btnChartPause.addEventListener("click", () => {
        chartPaused = !chartPaused;
        btnChartPause.textContent = chartPaused ? "▶ Resume" : "⏸ Pause";
    });
    
    const btnChartClear = document.getElementById("btn-chart-clear");
    btnChartClear.addEventListener("click", () => {
        chartTimeLabels.length = 0;
        targetData.length = 0;
        actualData.length = 0;
        errorData.length = 0;
        deltaErrorData.length = 0;
        fuzzyOutData.length = 0;
        startTime = null;
        
        chartAngles.update();
        chartFuzzyInputs.update();
        chartFuzzyOutput.update();
    });
    
    // Table controls
    const filterInput = document.getElementById("filter-history");
    filterInput.addEventListener("input", (e) => {
        filterHistoryTable(e.target.value);
    });
    
    const btnUploadCsv = document.getElementById("btn-upload-csv");
    const fileCsvInput = document.getElementById("file-csv-input");
    btnUploadCsv.addEventListener("click", () => {
        fileCsvInput.click();
    });
    
    fileCsvInput.addEventListener("change", (e) => {
        if (e.target.files.length > 0) {
            handleLocalCsvSelect(e.target.files[0]);
        }
    });
    
    const btnRequestEsp = document.getElementById("btn-request-esp");
    btnRequestEsp.addEventListener("click", requestCsvFromEsp32);
    
    const btnExportExcel = document.getElementById("btn-export-excel");
    btnExportExcel.addEventListener("click", exportToExcel);
    
    const btnClearTable = document.getElementById("btn-clear-table");
    btnClearTable.addEventListener("click", () => {
        sessionData = [];
        populateHistoryTable([]);
    });
}

// Toggle Koneksi Serial
async function toggleConnection() {
    const btnConnect = document.getElementById("btn-connect");
    
    if (isConnectedToSerial) {
        // Disconnect
        try {
            await fetch("/api/disconnect", { method: "POST" });
        } catch (err) {
            console.error("Gagal putuskan serial:", err);
        }
    } else {
        // Connect
        const portSelect = document.getElementById("port-select");
        const port = portSelect.value;
        const baud = parseInt(document.getElementById("baud-select").value);
        
        if (!port) {
            alert("Pilih port COM terlebih dahulu!");
            return;
        }
        
        btnConnect.disabled = true;
        btnConnect.textContent = "Menghubungkan...";
        
        try {
            const res = await fetch("/api/connect", {
                method: "POST",
                headers: {
                    "Content-Type": "application/json"
                },
                body: JSON.stringify({ port: port, baud_rate: baud })
            });
            
            if (!res.ok) {
                const errData = await res.json();
                throw new Error(errData.detail || "Gagal menghubungkan.");
            }
        } catch (err) {
            alert(`Koneksi Gagal: ${err.message}`);
            updateConnectionBadge(false);
        } finally {
            btnConnect.disabled = false;
        }
    }
}

// Minta Data CSV dari ESP32
async function requestCsvFromEsp32() {
    const activeConnection = (connectionMode === "usb" && isConnectedToSerial) || (connectionMode === "wifi" && isConnectedToWifi);
    if (!activeConnection) {
        alert("ESP32 tidak terhubung! Pastikan USB atau WiFi aktif.");
        return;
    }
    
    try {
        const res = await fetch("/api/request-csv", { method: "POST" });
        if (!res.ok) {
            const errData = await res.json();
            throw new Error(errData.detail || "Gagal mengirim permintaan.");
        }
    } catch (err) {
        alert(`Gagal minta CSV: ${err.message}`);
    }
}
