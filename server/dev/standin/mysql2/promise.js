// A stand-in for mysql2/promise, for trying the server's database mode on a
// PC without MySQL: it answers the handful of statements mcla-server.js makes
// and keeps its two tables in a JSON file named after the database, so a
// restart (or a fresh data folder) finds them again, as a real database would.
'use strict';
const fs = require('fs');
const path = require('path');

function createPool(settings) {
  const name = settings.uri ? new URL(settings.uri).pathname.replace(/^\//, '') : settings.database;
  const file = path.join(__dirname, '..', '..', `fakedb-${name || 'db'}.json`);
  let tables = { mcla_store: {}, mcla_files: {} };
  try { tables = JSON.parse(fs.readFileSync(file, 'utf8')); } catch (_) { /* new */ }
  const persist = () => fs.writeFileSync(file, JSON.stringify(tables));
  const log = [];
  async function query(sql, params = []) {
    log.push(sql.slice(0, 60));
    const s = sql.replace(/\s+/g, ' ').trim();
    if (s === 'SELECT 1') return [[{ 1: 1 }], []];
    if (/^CREATE TABLE IF NOT EXISTS (mcla_store|mcla_files)/.test(s)) return [[], []];
    let m;
    if ((m = /^SELECT version, data FROM mcla_store WHERE name = \?( AND version <> \?)?$/.exec(s))) {
      const row = tables.mcla_store[params[0]];
      if (!row) return [[], []];
      if (m[1] && row.version === params[1]) return [[], []];
      return [[{ version: row.version, data: row.data }], []];
    }
    if (/^INSERT INTO mcla_store \(name, version, data\) VALUES \(\?, \?, \?\)( ON DUPLICATE KEY UPDATE version = VALUES\(version\), data = VALUES\(data\))?$/.test(s)) {
      if (tables.mcla_store[params[0]] && !/ON DUPLICATE/.test(s)) throw new Error(`Duplicate entry '${params[0]}' for key 'PRIMARY'`);
      tables.mcla_store[params[0]] = { version: params[1], data: params[2] };
      persist();
      return [{ affectedRows: 1 }, []];
    }
    if (/^INSERT INTO mcla_files \(name, type, size, modified, data\) VALUES \(\?, \?, \?, \?, \?\) ON DUPLICATE KEY UPDATE/.test(s)) {
      tables.mcla_files[params[0]] = { type: params[1], size: params[2], modified: params[3], data: Buffer.from(params[4]).toString('base64') };
      persist();
      return [{ affectedRows: 1 }, []];
    }
    if (/^SELECT type, size, modified, data FROM mcla_files WHERE name = \?$/.test(s)) {
      const row = tables.mcla_files[params[0]];
      return [row ? [{ type: row.type, size: row.size, modified: row.modified, data: Buffer.from(row.data, 'base64') }] : [], []];
    }
    if (/^DELETE FROM mcla_files WHERE name = \?$/.test(s)) {
      const had = !!tables.mcla_files[params[0]];
      delete tables.mcla_files[params[0]];
      persist();
      return [{ affectedRows: had ? 1 : 0 }, []];
    }
    if (/^SELECT COUNT\(\*\) AS n FROM mcla_files$/.test(s)) return [[{ n: Object.keys(tables.mcla_files).length }], []];
    throw new Error(`the stand-in does not know this statement: ${s}`);
  }
  return { query, execute: query, end: async () => {}, _log: log, _file: file };
}

module.exports = { createPool };
