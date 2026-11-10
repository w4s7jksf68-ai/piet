// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// One replaceable transient status message.
let _statusFlashTimer = null;

export function flashStatus(msg, ms = 2500) {
  const el = document.getElementById('status-flash');
  if (!el) return;
  clearTimeout(_statusFlashTimer);
  el.textContent = msg;
  el.hidden = false;
  _statusFlashTimer = setTimeout(() => { el.hidden = true; el.textContent = ''; }, ms);
}
