#!/usr/bin/env node
// Status line: model | branch (* = uncommitted changes) | context used | version (+ commits since the last release).
'use strict';
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

let s = {};
try { s = JSON.parse(fs.readFileSync(0, 'utf8')); } catch { /* no input: show what we can */ }
const ws = s.workspace || {};
const dir = ws.current_dir || s.cwd || process.cwd();
const git = (...a) => {
  try { return execFileSync('git', a, { cwd: dir, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'], timeout: 2000 }).trim(); }
  catch { return ''; }
};

const parts = [(s.model && s.model.display_name) || 'Claude'];
const branch = git('branch', '--show-current');
if (branch) parts.push(branch + (git('status', '--porcelain', '-uno') ? '*' : '')); // -uno: big repos stay fast

const cw = s.context_window || {};
let pct = cw.used_percentage;
if (pct == null && cw.current_usage && cw.context_window_size) {
  const u = cw.current_usage;
  pct = (100 * ((u.input_tokens || 0) + (u.cache_creation_input_tokens || 0) + (u.cache_read_input_tokens || 0))) / cw.context_window_size;
}
if (typeof pct === 'number') parts.push(`ctx ${Math.round(pct)}%`);

// Version: the VERSION file, else the last release tag; "+12" = commits since that tag (time to suggest a bump?).
// shortcut: only a VERSION file is read; add package.json etc. if a project keeps its version elsewhere.
let version = '';
try { version = fs.readFileSync(path.join(ws.project_dir || dir, 'VERSION'), 'utf8').split(/\r?\n/)[0].trim(); } catch { /* none */ }
const tag = git('describe', '--tags', '--abbrev=0');
if (!version) version = tag;
if (version) {
  const since = tag ? Number(git('rev-list', '--count', `${tag}..HEAD`)) : 0;
  parts.push('v' + version.replace(/^v/i, '') + (since ? ` +${since}` : ''));
}

process.stdout.write(parts.join(' | '));
