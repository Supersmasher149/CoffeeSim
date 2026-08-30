import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// The native tool server owns every calculation; the dev server only proxies to
// it so the browser never becomes an authoritative source (section 3.1).
// The target is configurable so Playwright (web/e2e/) can point a Vite
// instance at a native server it started on its own isolated port, without
// disturbing port 8734 as everyone else's default.
const proxyTarget = process.env.ESPRESSOLAB_SERVER_URL ?? "http://127.0.0.1:8734";

export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      "/api": {
        target: proxyTarget,
        changeOrigin: true,
      },
    },
  },
});
