// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Native save dialogs and browser downloads for Results and graph images.
import { appendBlock, clearConsole } from './console.js';

export function createFileExportController({ getClient, composeDefaultPath, flashStatus }) {
  function _blobToBase64(blob) {
    return new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = () => {
        const result = reader.result || '';
        const idx = result.indexOf(',');
        resolve(idx >= 0 ? result.slice(idx + 1) : result);
      };
      reader.onerror = () => reject(reader.error || new Error('FileReader failed'));
      reader.readAsDataURL(blob);
    });
  }

  function svgToPng(svgEl, filename) {
    try {
      // viewBox "0 0 W H" gives the natural pixel size; fall back to 700x440 (graph.js).
      const vb = (svgEl.getAttribute('viewBox') || '0 0 700 440').split(/\s+/).map(Number);
      const W = vb[2] || 700, H = vb[3] || 440;
      // The graph <svg> is themed entirely through piet.css classes (.g-frame,
      // .g-point, .g-ticklabel, …) and CSS variables — a serialized <svg> carries
      // NO external stylesheet, so once it is rasterized standalone (data: URL →
      // <img> → canvas) every class-based fill/stroke collapses to the SVG default
      // (black): the .g-frame panel fills solid black and the points disappear
      // into it (reported on Windows, but the raster loses the CSS on every
      // platform). Flatten the resolved paint props onto a clone — getComputedStyle
      // must read the LIVE node, so read from svgEl and write to the clone — and
      // bake the themed background (dark mode paints .g-svg via CSS `background`,
      // also dropped) as a real rect, so the PNG matches what is on screen.
      const clone = svgEl.cloneNode(true);
      const src = [svgEl, ...svgEl.querySelectorAll('*')];
      const dst = [clone, ...clone.querySelectorAll('*')];
      const PAINT = ['fill', 'stroke', 'stroke-width', 'stroke-dasharray', 'stroke-linejoin',
                     'stroke-linecap', 'opacity', 'fill-opacity', 'stroke-opacity',
                     'font-size', 'font-family', 'font-weight', 'text-anchor'];
      for (let i = 0; i < src.length && i < dst.length; i++) {
        const cs = getComputedStyle(src[i]);
        for (const p of PAINT) {
          const v = cs.getPropertyValue(p);
          if (v) dst[i].style.setProperty(p, v);
        }
      }
      const bg = getComputedStyle(svgEl).backgroundColor;
      if (bg && bg !== 'transparent' && bg !== 'rgba(0, 0, 0, 0)') {
        const r = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
        r.setAttribute('x', '0'); r.setAttribute('y', '0');
        r.setAttribute('width', String(W)); r.setAttribute('height', String(H));
        r.setAttribute('fill', bg);
        clone.insertBefore(r, clone.firstChild);
      }
      const xml = new XMLSerializer().serializeToString(clone);
      const svg64 = btoa(unescape(encodeURIComponent(xml)));
      const img = new Image();
      img.onload = () => {
        const scale = 2;  // crisp export
        const canvas = document.createElement('canvas');
        canvas.width = W * scale; canvas.height = H * scale;
        const ctx = canvas.getContext('2d');
        ctx.fillStyle = '#ffffff';
        ctx.fillRect(0, 0, canvas.width, canvas.height);
        ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
        canvas.toBlob(async blob => {
          if (!blob) return;
          // N1: real save when a native bridge exists — feature-detected so a
          // plain browser tab / the DOM-fixture mock keeps the anchor-download
          // path below byte-unchanged.
          if (getClient() && getClient().native) {
            try {
              const content_b64 = await _blobToBase64(blob);
              const res = await getClient().saveFile({
                suggested_name: filename, kind: 'png', content_b64,
                path: composeDefaultPath(filename),
              });
              if (res && res.ok) flashStatus(`Saved ${res.path}`);
              // {ok:false, cancelled:true} (user dismissed NSSavePanel) — quiet no-op.
            } catch (err) {
              appendBlock({ type: 'error', code: 'engine_error', message: `PNG export failed: ${err.message ?? err}` });
            }
            return;
          }
          const url = URL.createObjectURL(blob);
          const a = document.createElement('a');
          a.href = url; a.download = filename;
          a.click();
          URL.revokeObjectURL(url);
        }, 'image/png');
      };
      img.src = 'data:image/svg+xml;base64,' + svg64;
    } catch (err) {
      appendBlock({ type: 'error', code: 'engine_error', message: `PNG export failed: ${err.message ?? err}` });
    }
  }

  function onClear() {
    clearConsole();
  }

  async function onSaveResults() {
    const body = document.getElementById('console-body');
    // Walk the block children (not body.textContent directly) so each block
    // gets a blank-line separator — plain textContent concatenation across
    // sibling <div>s runs everything together with no line breaks.
    const blocks = body ? Array.from(body.children).filter(el => el.classList.contains('block')) : [];
    // A10 (P8.1): strip UI action bars (Copy table / Export CSV — marked
    // .console-action, incl. the pre-existing esttab bar) from the saved text;
    // they are chrome, not results. Clone-and-remove so the live DOM is untouched.
    const blockText = (b) => {
      if (!b.querySelector('.console-action')) return b.textContent;
      const clone = b.cloneNode(true);
      clone.querySelectorAll('.console-action').forEach(el => el.remove());
      return clone.textContent;
    };
    const text = blocks.map(blockText).join('\n\n').trim();
    if (!text) {
      flashStatus('Nothing to save — the results log is empty');
      return;
    }
    const d = new Date();
    const pad = n => String(n).padStart(2, '0');
    const stamp = `${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}-${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`;
    const filename = `piet-results-${stamp}.log`;

    if (getClient() && getClient().native) {
      try {
        const res = await getClient().saveFile({
          suggested_name: filename, kind: 'log', text: text + '\n',
          path: composeDefaultPath(filename),
        });
        if (res && res.ok) flashStatus(`Saved ${res.path}`);
        // {ok:false, cancelled:true} (user dismissed NSSavePanel) — quiet no-op.
      } catch (err) {
        appendBlock({ type: 'error', code: 'engine_error', message: `Save Results failed: ${err.message ?? err}` });
      }
      return;
    }

    const blob = new Blob([text + '\n'], { type: 'text/plain' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);

    flashStatus(`Saved ${filename}`);
  }

  function bindControls() {
    // Clear button
    const clearBtn = document.getElementById('btn-clear');
    if (clearBtn) clearBtn.addEventListener('click', onClear);

    // B2 (P8-S6): Save Results — serialize the results log to a .log download.
    const saveResultsBtn = document.getElementById('btn-save-results');
    if (saveResultsBtn) saveResultsBtn.addEventListener('click', onSaveResults);
  }

  return { _blobToBase64, svgToPng, onClear, onSaveResults, bindControls };
}
