import { defineConfig } from 'vite';
import path from 'node:path';

export default defineConfig({
  base: '/',
  publicDir: 'pwa',
  build: {
    outDir: path.resolve(__dirname, '../data'),
    emptyOutDir: true,
    sourcemap: false,
    target: 'es2019'
  }
});
