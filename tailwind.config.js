/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{ts,js,html}'],
  theme: {
    extend: {
      fontFamily: {
        game: ['"Chakra Petch"', '"Segoe UI"', 'system-ui', 'sans-serif'],
        mono: ['"JetBrains Mono"', 'ui-monospace', 'monospace'],
      },
      colors: {
        vx: {
          bg: '#07090f',
          panel: 'rgba(10,14,24,0.82)',
          line: '#1e2a44',
          accent: '#39ff88',
          accent2: '#37c6ff',
          warn: '#ffb020',
          danger: '#ff4d6d',
        },
      },
      boxShadow: {
        glow: '0 0 24px rgba(57,255,136,0.28)',
        panel: '0 10px 40px rgba(0,0,0,0.55)',
      },
      keyframes: {
        pulseSlow: { '0%,100%': { opacity: '0.55' }, '50%': { opacity: '1' } },
        floaty: { '0%,100%': { transform: 'translateY(0)' }, '50%': { transform: 'translateY(-4px)' } },
      },
      animation: { pulseSlow: 'pulseSlow 2.6s ease-in-out infinite', floaty: 'floaty 4s ease-in-out infinite' },
    },
  },
  plugins: [],
};
