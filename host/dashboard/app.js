/* Relais live dashboard front-end (OBS-06, OBS-08). No external library:
   plain DOM + canvas. Two modes share every render function:
   - live: an EventSource tails the journal through /api/stream;
   - replay: snapshot.py embeds window.RELAIS_REPLAY and we step it by hand.
*/
(function () {
  "use strict";

  var SOURCE_COLORS = {
    holder: "#2fbf71", shadow: "#38c6d9", testament: "#e0a92e",
    fallback: "#e05555", none: "#5b6b7c"
  };
  var MAX_POINTS = 300;
  var replay = (window.RELAIS_REPLAY && window.RELAIS_REPLAY.length) || [];
  var mode = replay.length ? "replay" : "live";

  var ticks = [], values = [], positions = [], sources = [];
  var events = [];
  var worstDev = 0;
  var replayIndex = 0, playing = false, replayTimer = null;

  function el(id) { return document.getElementById(id); }
  function fmt(v) {
    if (v === null || v === undefined || Number.isNaN(v)) return "—";
    var n = Number(v);
    if (Math.abs(n) >= 1000) return n.toFixed(0);
    if (Math.abs(n) >= 10) return n.toFixed(2);
    return n.toFixed(3);
  }

  function setLamp(lamp, cause) {
    var lampEl = el("lamp");
    lampEl.className = "lamp" + (lamp === "off" ? "" : " " + lamp);
    if (lamp === "off") el("cause").textContent = cause || "witness off";
    else el("cause").textContent = cause || lamp;
  }
  function setSource(source) {
    var b = el("source");
    b.className = "source-badge " + (SOURCE_COLORS[source] ? source : "none");
    b.textContent = source || "none";
  }
  function setStatus(live) {
    var s = el("status");
    s.className = "status " + (live ? "live" : "dead");
    el("status-text").textContent = live ? "live" : "disconnected";
  }

  function pushPoint(tick, value, pos, source) {
    ticks.push(tick); values.push(value); positions.push(pos);
    sources.push(source || "none");
    if (values.length > MAX_POINTS) {
      ticks.shift(); values.shift(); positions.shift(); sources.shift();
    }
  }

  function applyRecord(rec) {
    if (rec.kind === "tick") {
      el("mandate").textContent = rec.mandate;
      el("holder").textContent = rec.holder || "—";
      el("tick").textContent = rec.tick;
      el("cmd").textContent = fmt(rec.value);
      el("pos").textContent = fmt(rec.pos);
      el("setpoint").textContent = fmt(rec.setpoint);
      el("misses").textContent = rec.misses;
      el("twin-dev").textContent = (rec.twin_dev === null || rec.twin_dev === undefined)
        ? "—" : fmt(rec.twin_dev);
      if (typeof rec.twin_dev === "number") worstDev = Math.max(worstDev, rec.twin_dev);
      setSource(rec.source);
      setLamp(rec.lamp, rec.cause);
      pushPoint(rec.tick, rec.value, rec.pos, rec.source);
      drawCharts();
    } else if (rec.kind === "event") {
      prependEvent(rec);
    } else if (rec.kind === "header") {
      el("scenario").textContent = rec.scenario || "";
    }
  }

  function applyState(state) {
    if (state.scenario) el("scenario").textContent = state.scenario;
    if (state.last && state.last.tick !== undefined) {
      el("tick").textContent = state.last.tick;
      el("mandate").textContent = state.last.mandate;
      el("holder").textContent = state.last.holder || "—";
      el("cmd").textContent = fmt(state.last.value);
      el("pos").textContent = fmt(state.last.pos);
      el("setpoint").textContent = fmt(state.last.setpoint);
      el("misses").textContent = state.last.misses;
      el("twin-dev").textContent = (state.last.twin_dev === null || state.last.twin_dev === undefined)
        ? "—" : fmt(state.last.twin_dev);
      setSource(state.last.source);
      setLamp(state.last.lamp, state.last.cause);
    }
    var v = state.violations || 0;
    el("violations").textContent = v;
    el("viol-kpi").className = "kpi violations" + (v === 0 ? " zero" : "");
    el("feed").innerHTML = "";
    events = [];
    (state.events || []).slice().reverse().forEach(prependEvent);
  }

  function prependEvent(rec) {
    events.push(rec);
    var li = document.createElement("li");
    var t = document.createElement("span");
    t.className = "t"; t.textContent = "t" + rec.tick;
    var e = document.createElement("span");
    e.className = "ev"; e.textContent = rec.event;
    li.appendChild(t); li.appendChild(e);
    if (rec.node) li.appendChild(document.createTextNode(" " + rec.node));
    if (rec.mandate) li.appendChild(document.createTextNode(" · mandat " + rec.mandate));
    if (rec.text && rec.text !== "none") li.appendChild(document.createTextNode(" · " + rec.text));
    var feed = el("feed");
    feed.insertBefore(li, feed.firstChild);
    while (feed.childNodes.length > 100) feed.removeChild(feed.lastChild);
  }

  function chartExtent(arr) {
    var lo = Infinity, hi = -Infinity, i;
    for (i = 0; i < arr.length; i++) {
      var v = Number(arr[i]);
      if (!Number.isFinite(v)) continue;
      if (v < lo) lo = v; if (v > hi) hi = v;
    }
    if (!Number.isFinite(lo)) { lo = -1; hi = 1; }
    if (hi - lo < 1e-9) { lo -= 0.5; hi += 0.5; }
    var pad = (hi - lo) * 0.08 || 0.1;
    return [lo - pad, hi + pad];
  }

  function drawCurve(canvasId, series, color, fill) {
    var c = el(canvasId);
    if (!c) return;
    var dpr = window.devicePixelRatio || 1;
    var w = c.clientWidth || 1, h = c.clientHeight || 170;
    c.width = w * dpr; c.height = h * dpr;
    var ctx = c.getContext("2d");
    ctx.scale(dpr, dpr);
    ctx.clearRect(0, 0, w, h);
    ctx.font = "12px ui-monospace, Menlo, monospace";
    var n = series.length;
    if (n === 0) return;
    var ext = chartExtent(series);
    var m = 8, plotW = w - 2 * m, plotH = h - 2 * m;
    ctx.strokeStyle = "#1c2834"; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(m, m + plotH / 2); ctx.lineTo(m + plotW, m + plotH / 2);
    ctx.fillStyle = "#5b6b7c";
    ctx.fillText(ext[1].toFixed(3), 2, m - 2);
    ctx.fillText(ext[0].toFixed(3), 2, m + plotH);
    var x = function (i) { return m + plotW * (i / (n - 1)); };
    var y = function (v) { return m + plotH * (1 - (v - ext[0]) / (ext[1] - ext[0])); };
    ctx.strokeStyle = "#1c2834"; ctx.stroke();
    var lastX = null, lastY = null;
    for (var i = 0; i < n; i++) {
      var v = Number(series[i]);
      if (!Number.isFinite(v)) continue;
      var px = x(i), py = y(v);
      ctx.strokeStyle = color(i);
      if (lastX !== null) {
        ctx.beginPath(); ctx.moveTo(lastX, lastY); ctx.lineTo(px, py); ctx.stroke();
      }
      lastX = px; lastY = py;
    }
    ctx.fillStyle = "rgba(58,160,255,0.08)";
    if (fill === "area") {
      ctx.beginPath(); ctx.moveTo(x(0), y(Number(series[0])));
      for (var j = 1; j < n; j++) ctx.lineTo(x(j), y(Number(series[j])));
      ctx.lineTo(x(n - 1), m + plotH); ctx.lineTo(x(0), m + plotH); ctx.closePath(); ctx.fill();
    }
  }

  function drawCharts() {
    drawCurve("cmd-chart", values, function (i) { return SOURCE_COLORS[sources[i]] || "#5b6b7c"; }, "area");
    drawCurve("pos-chart", positions, function () { return "#3aa0ff"; }, "none");
  }

  function connect() {
    var es = new EventSource("/api/stream");
    es.addEventListener("state", function (e) { applyState(JSON.parse(e.data)); setStatus(true); });
    es.addEventListener("record", function (e) { applyRecord(JSON.parse(e.data)); });
    es.onerror = function () { setStatus(false); };
  }

  function stepReplay() {
    if (replayIndex >= replay.length) { setPlaying(false); return; }
    var rec = replay[replayIndex++];
    if (rec && rec.kind) applyRecord(rec);
    renderReplayPos();
  }
  function renderReplayPos() {
    el("rpos").textContent = replayIndex + " / " + replay.length;
    el("scrub").value = replayIndex;
  }
  function setPlaying(on) {
    playing = on;
    el("play").textContent = on ? "❚❚ pause" : "▶ play";
    if (on && !replayTimer) replayTimer = setInterval(stepReplay, 80);
    if (!on && replayTimer) { clearInterval(replayTimer); replayTimer = null; }
  }

  function initReplayControls() {
    document.body.className = "replay-mode";
    el("scrub").max = replay.length;
    el("play").addEventListener("click", function () {
      if (replayIndex >= replay.length) replayIndex = 0;
      setPlaying(!playing);
    });
    el("scrub").addEventListener("input", function () {
      var to = parseInt(el("scrub").value, 10);
      replayIndex = Math.min(to, replay.length);
      renderReplayPos();
    });
    // Header records arrive before ticks; initialise from the first one.
    for (var i = 0; i < replay.length; i++) {
      if (replay[i] && replay[i].kind === "header") { applyRecord(replay[i]); break; }
    }
    renderReplayPos();
  }

  window.addEventListener("load", function () {
    if (mode === "replay") initReplayControls();
    else { connect(); }
  });
})();