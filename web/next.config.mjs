/** @type {import('next').NextConfig} */
const nextConfig = {
  output: "standalone",
  // ssh2 (used by the server-side onboarding route) ships native crypto bindings
  // that cannot be bundled into an ESM chunk. Keep it external so it is loaded as
  // a Node module at runtime on the server only.
  serverExternalPackages: ["ssh2"]
};

export default nextConfig;
