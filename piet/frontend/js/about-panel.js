// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// About/legal overlay; notices are cached on its current DOM element.

export function createAboutController({ getClient, layout }) {
  const { closeAllOverlays, syncViewTabs, _isOverlayOpen } = layout;

  async function openAbout() {
    const ov = document.getElementById('about-overlay');
    if (!ov) return;
    // Mission 1: close whichever other overlay shares the console area first.
    closeAllOverlays('about-overlay');
    ov.classList.add('open');
    // UX-047: About isn't a view tab itself, but it's still an overlay — no
    // tab is active while it's open ("Results = no overlay open").
    syncViewTabs();

    // Version line (best-effort; absent in mock/browser sessions).
    const verEl = document.getElementById('about-version');
    if (verEl) {
      try {
        const v = await getClient().version();
        verEl.textContent = (v && v.piet_version)
          ? `PIET ${v.piet_version} · engine CoBrA ${v.cobra_version}`
          : 'PIET (no kernel connected)';
      } catch {
        verEl.textContent = 'PIET (no kernel connected)';
      }
    }

    // Third-party notices — fetched once per session, then kept on the element.
    const pre = document.getElementById('about-notices');
    if (pre && !pre.dataset.loaded) {
      const fallback = 'Third-party notices ship with the packaged app as '
        + 'Resources/THIRD_PARTY_NOTICES.txt (not available in this browser/mock session).';
      pre.textContent = 'Loading third-party notices…';
      try {
        const n = await getClient().legalNotices();
        if (n && n.text) {
          pre.textContent = n.text;
          pre.dataset.loaded = '1';
        } else {
          pre.textContent = fallback;
        }
      } catch {
        pre.textContent = fallback;
      }
    }
  }

  function closeAbout() {
    const ov = document.getElementById('about-overlay');
    if (ov) ov.classList.remove('open');
    document.getElementById('cmd-input')?.focus();
    syncViewTabs();   // UX-047: Results tab active again (if nothing else is open)
  }

  function bindControls() {
    // About / Legal overlay (WI-5 acknowledgements + WI-6 disclaimer)
    // BUG-095 (P8.1-T1): the entry link is the exit too — re-click toggles the
    // panel closed. Applied to About as well (same trap class: neither is a
    // view tab, so without this the only exits were the undiscoverable Escape
    // or knowing to click a view tab).
    document.getElementById('about-close')?.addEventListener('click', closeAbout);
    const aboutLink = document.querySelector('[data-action="about"]');
    if (aboutLink) aboutLink.addEventListener('click', e => {
      e.preventDefault();
      if (_isOverlayOpen('about-overlay')) closeAbout(); else openAbout();
    });
  }

  return { openAbout, closeAbout, bindControls };
}
