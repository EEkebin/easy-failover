import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "easy-failover Dashboard",
  description: "Read-only local dashboard for easy-failover state"
};

export default function RootLayout({
  children
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="en">
      <body>{children}</body>
    </html>
  );
}
