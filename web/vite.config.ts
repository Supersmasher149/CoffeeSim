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
  resolve: {
    alias: [
      // recharts imports individual lodash submodules (lodash/get, lodash/isNumber,
      // ...); lodash's CJS internals require() shared helpers that Rollup's CJS
      // interop can't tree-shake, so the whole library rides along for ~28 used
      // functions. lodash-es ships the same functions as real ESM, so only what's
      // actually imported ends up in the bundle. This doesn't touch our own code --
      // we have no direct lodash usage.
      { find: /^lodash\/(.*)$/, replacement: "lodash-es/$1" },
      { find: "lodash", replacement: "lodash-es" },
    ],
  },
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
