import { copyFileSync, mkdirSync, statSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const source = resolve(
  repoRoot,
  'game/Pokemon - Yellow Version - Special Pikachu Edition (USA, Europe) (GBC,SGB Enhanced).srm'
);
const destination = resolve(repoRoot, 'data/save.srm');

const { size } = statSync(source);
if (size !== 32768) {
  throw new Error(`Default save must be 32768 bytes, got ${size}: ${source}`);
}

mkdirSync(dirname(destination), { recursive: true });
copyFileSync(source, destination);
console.log(`Copied default save to ${destination}`);
