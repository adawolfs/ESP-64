import { defineConfig } from 'vite';
import path from 'node:path';

export default defineConfig({
  base: './',
  publicDir: 'pwa',
  build: {
    outDir: path.resolve(__dirname, '../data'),
    emptyOutDir: true,
    sourcemap: false,
    target: 'es2019',
    rollupOptions: {
      output: {
        entryFileNames: 'assets/index.js',
        chunkFileNames: 'assets/[name].js',
        assetFileNames: (assetInfo) => {
          if (assetInfo.name && assetInfo.name.endsWith('.css')) return 'assets/index.css';
          return 'assets/[name][extname]';
        }
      }
    }
  }
});
