// Web UI — shared utilities

function applyAccent(hex) {
  var r = parseInt(hex.slice(1,3), 16);
  var g = parseInt(hex.slice(3,5), 16);
  var b = parseInt(hex.slice(5,7), 16);
  var s = document.documentElement.style;
  s.setProperty('--accent', hex);
  s.setProperty('--accent-dim', 'rgb(' + (r*.6|0) + ',' + (g*.6|0) + ',' + (b*.6|0) + ')');
  s.setProperty('--accent-glow', 'rgba(' + r + ',' + g + ',' + b + ',0.12)');
  s.setProperty('--accent-glow-md', 'rgba(' + r + ',' + g + ',' + b + ',0.25)');
  s.setProperty('--accent-bg', 'rgba(' + r + ',' + g + ',' + b + ',0.03)');
}

async function loadBrand() {
  try {
    var about = await api('GET', '/api/about');
    if (about.brand) {
      var name = about.brand.name || 'Device';
      document.title = name + (document.title.includes('-')
        ? ' -' + document.title.split('-').slice(1).join('-') : '');
      var h1 = document.querySelector('header h1');
      if (h1) h1.textContent = name;
      if (about.brand.accent) applyAccent(about.brand.accent);
      window._brand = about.brand;
    }
  } catch(e) {}
}

async function api(method, path, body) {
  const opts = {
    method: method,
    headers: {}
  };
  if (body !== undefined) {
    opts.headers['Content-Type'] = 'application/json';
    opts.body = JSON.stringify(body);
  }
  const res = await fetch(path, opts);
  if (!res.ok) {
    const text = await res.text();
    throw new Error(text || res.statusText);
  }
  const ct = res.headers.get('content-type') || '';
  if (ct.includes('application/json')) {
    return res.json();
  }
  return res.text();
}

function $(sel) { return document.querySelector(sel); }
function $$(sel) { return document.querySelectorAll(sel); }

function showToast(msg, type) {
  let t = $('#toast');
  if (!t) {
    t = document.createElement('div');
    t.id = 'toast';
    t.className = 'toast';
    document.body.appendChild(t);
  }
  t.textContent = msg;
  t.className = 'toast ' + (type || 'success') + ' show';
  clearTimeout(t._timer);
  t._timer = setTimeout(function() {
    t.classList.remove('show');
  }, 3000);
}

function formatBytes(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1048576) return (bytes / 1024).toFixed(1) + ' KB';
  return (bytes / 1048576).toFixed(1) + ' MB';
}

function formatUptime(ms) {
  var s = Math.floor(ms / 1000);
  var d = Math.floor(s / 86400); s %= 86400;
  var h = Math.floor(s / 3600); s %= 3600;
  var m = Math.floor(s / 60); s %= 60;
  var parts = [];
  if (d) parts.push(d + 'd');
  if (h) parts.push(h + 'h');
  if (m) parts.push(m + 'm');
  parts.push(s + 's');
  return parts.join(' ');
}
