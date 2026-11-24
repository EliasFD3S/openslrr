import { useCountUp, useInView } from "../hooks.js"

// A tachometer in the style of the OpenSLRR logo: grey ticks turning orange
// then red near the top of the range. When it first scrolls into view, CSS
// sweeps the needle (a rotated <g>) and lights the ticks in turn; only the
// number counts up in JS.
export default function Gauge({ value, max, label }) {
  const [ref, seen] = useInView({ threshold: 0.4 })
  const shown = useCountUp(value, seen, 1800)
  const start = -225 // degrees, 0 = pointing right
  const sweep = 270
  const ticks = 40
  const cx = 120
  const cy = 120
  const ratio = Math.min(value / max, 1)
  const angle = (t) => ((start + sweep * t) * Math.PI) / 180
  const point = (t, r) => [cx + Math.cos(angle(t)) * r, cy + Math.sin(angle(t)) * r]
  const [nx, ny] = point(0, 86)
  return (
    <figure className={`gauge ${seen ? "is-visible" : ""}`} ref={ref} style={{ "--sweep": `${sweep * ratio}deg` }}>
      <svg viewBox="0 0 240 240" role="img" aria-label={`${label}: ${value} of ${max}`}>
        <defs>
          <radialGradient id="gauge-face" cx="50%" cy="38%" r="68%">
            <stop offset="0%" stopColor="#1f1f22" />
            <stop offset="100%" stopColor="#050505" />
          </radialGradient>
        </defs>
        <circle cx={cx} cy={cy} r="114" className="gauge-rim" />
        <circle cx={cx} cy={cy} r="110" fill="url(#gauge-face)" />
        {Array.from({ length: ticks + 1 }, (_, i) => {
          const t = i / ticks
          const major = i % 5 === 0
          const [x1, y1] = point(t, major ? 80 : 89)
          const [x2, y2] = point(t, 102)
          const tone = t > 0.9 ? "red" : t > 0.78 ? "orange" : "grey"
          return (
            <line
              key={i}
              x1={x1}
              y1={y1}
              x2={x2}
              y2={y2}
              className={`tick tick-${tone}${t <= ratio ? " lit" : ""}`}
              strokeWidth={major ? 5 : 3}
              style={{ "--t": t }}
            />
          )
        })}
        <g className="needle">
          {/* A full-size invisible disc makes the group's box centre the dial centre. */}
          <circle cx={cx} cy={cy} r="110" fill="none" stroke="none" />
          <line x1={cx} y1={cy} x2={nx} y2={ny} className="needle-line" />
        </g>
        <circle cx={cx} cy={cy} r="10" className="hub" />
      </svg>
      <figcaption>
        <span className="gauge-value">{Math.round(shown)}</span>
        <span className="gauge-max">/ {max}</span>
        <span className="gauge-label">{label}</span>
      </figcaption>
    </figure>
  )
}
