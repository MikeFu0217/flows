import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  base: './', plugins: [react()],
  build: { target: 'es2020', cssCodeSplit: false,
    rollupOptions: { output: { entryFileNames: 'assets/index.js', chunkFileNames: 'assets/[name].js',
      assetFileNames: asset => asset.name?.endsWith('.css') ? 'assets/index.css' : 'assets/[name][extname]' } } },
});
