import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// The native tool server owns every calculation; the dev server only proxies to
// it so the browser never becomes an authoritative source (section 3.1).
export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      "/api": {
        target: "http://127.0.0.1:8734",
        changeOrigin: true,
      },
    },
  },
});
