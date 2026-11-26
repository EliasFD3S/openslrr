import { useRef, useState } from "react"
import Gauge from "./components/Gauge.jsx"
import { useCountUp, useInView } from "./hooks.js"
import { Footer, Header, REPO, useInitialFragment } from "./SiteLayout.jsx"

// No prebuilt release yet: Download leads to the build guide. When the first
// release is out, point DOWNLOAD at `${REPO}/releases/latest`.
const DOWNLOAD = "./docs/#/build"
const BUILD_GUIDE = "./docs/#/build"
const DOCS = "./docs/"
const NB = " " // keeps a number and its unit together

const FEATURES = [
  {
    tag: "VM",
    title: "The original scripts",
    text: "A purpose-built virtual machine runs the game’s own Java classes: career, garage, dealers, races and menus behave as they were written.",
  },
  {
    tag: "DX11",
    title: "A modern renderer",
    text: "Direct3D 11 through bgfx: cascaded sun shadows, ambient occlusion, street lamps and headlight beams that light and shadow the scene, live reflections and volumetric clouds.",
  },
  {
    tag: `120${NB}Hz`,
    title: "Vehicle dynamics",
    text: "Tyres, suspension, sway bars, aerodynamics, nitrous and body damage, simulated at 120 Hz on a physics thread of their own.",
  },
  {
    tag: "City",
    title: "A living Valo City",
    text: "Traffic, pedestrians and race opponents share the streets with you, from downtown to the expressway, by day and by night.",
  },
  {
    tag: "x64",
    title: "Fast on modest PCs",
    text: "Rendering is spread over every core. Every change is measured on a recorded drive, on all cores and on four efficiency cores, so it runs well beyond high-end desktops.",
  },
  {
    tag: "Mods",
    title: "Your game, your mods",
    text: "Runs on your SLRR 2.2.1 MWM install, built for its add-on cars and maps. Saves are readable JSON; wheels and pads go through SDL3.",
  },
]

const SHOTS = [
  { src: "./shots/downtown.jpg", title: "Downtown", alt: "Downtown Valo City by day, pedestrians on the pavement" },
  { src: "./shots/bridge.jpg", title: "The bridge", alt: "Driving towards the steel bridge downtown" },
  { src: "./shots/suburb-turn.jpg", title: "Residential streets", alt: "Turning through a residential street, a house and trees behind" },
  { src: "./shots/suburb.jpg", title: "Suburbs", alt: "The suburbs, traffic lights and houses along the road" },
  { src: "./shots/night.jpg", title: "Night", alt: "Downtown at night under the street lamps" },
]

const STATS = [
  { value: 358, suffix: "", label: "Original natives implemented", decimals: 0 },
  { value: 120, suffix: `${NB}Hz`, label: "Vehicle physics", decimals: 0 },
  { value: 3.96, suffix: `${NB}ms`, label: "Frame on a recorded city drive", decimals: 2 },
  { value: 64, suffix: "-bit", label: "Windows build", decimals: 0 },
]

// Adds `is-visible` once the element scrolls into view; CSS does the motion.
function Reveal({ as: Tag = "div", className = "", index = 0, children, ...rest }) {
  const [ref, seen] = useInView({ threshold: 0.15 })
  return (
    <Tag
      ref={ref}
      className={`reveal ${seen ? "is-visible" : ""} ${className}`}
      style={{ "--i": index }}
      {...rest}
    >
      {children}
    </Tag>
  )
}

function Hero() {
  return (
    <section className="hero" id="top" aria-labelledby="hero-title">
      <div className="hero-bg" aria-hidden="true" />
      <div className="hero-glow" aria-hidden="true" />
      <div className="hero-content">
        <h1 id="hero-title" className="hero-title">
          <img className="hero-logo" src="./img/logo.png" alt="OpenSLRR DX11" width="960" height="455" fetchpriority="high" />
        </h1>
        <p className="hero-line">
          An open-source engine for <strong translate="no">Street Legal Racing: Redline</strong>. The original
          scripts and cars, on a modern 64-bit Direct3D{NB}11 renderer.
        </p>
        <div className="actions">
          <a className="pill pill-primary" href={DOWNLOAD}>
            Download
          </a>
          <a className="pill" href="./launcher/">Launcher</a>
          <a className="pill" href={DOCS}>Read the docs</a>
          <a className="pill pill-ghost" href={REPO} target="_blank" rel="noreferrer">Source on GitHub</a>
        </div>
        <p className="hero-note">Free &amp; open source (MIT). You need your own copy of SLRR{NB}2.2.1 MWM.</p>
      </div>
      <a className="scroll-cue" href="#overview" aria-label="Scroll to the overview">
        <span aria-hidden="true" />
      </a>
    </section>
  )
}

function Stat({ value, suffix, label, decimals, index }) {
  const [ref, seen] = useInView({ threshold: 0.5 })
  const shown = useCountUp(value, seen, 1300)
  return (
    <div ref={ref} className={`stat reveal ${seen ? "is-visible" : ""}`} style={{ "--i": index }}>
      <span className="stat-value">
        {shown.toFixed(decimals)}
        <small>{suffix}</small>
      </span>
      <span className="stat-label">{label}</span>
    </div>
  )
}

function Stats() {
  return (
    <section className="stats" id="overview" aria-label="Key figures">
      <div className="wrap stats-grid">
        {STATS.map((s, i) => (
          <Stat key={s.label} {...s} index={i} />
        ))}
      </div>
    </section>
  )
}

function Status() {
  return (
    <section className="band status" aria-labelledby="status-title">
      <div className="wrap status-grid">
        <Gauge value={358} max={376} label="Natives implemented" />
        <Reveal className="status-panel">
          <h2 id="status-title" className="section-title">Under the hood</h2>
          <p>
            SLRR’s scripts call 376 native functions of the original engine. OpenSLRR implements 358 of them, and 3
            more do nothing on purpose. Still to come: water surfaces, the particle system and three welder calls.
          </p>
          <dl className="readout">
            <div><dt>Renderer</dt><dd>Direct3D{NB}11 · bgfx</dd></div>
            <div><dt>Physics</dt><dd>120{NB}Hz · own thread</dd></div>
            <div><dt>Platform</dt><dd>Windows 10 / 11 x64</dd></div>
            <div><dt>Licence</dt><dd>MIT</dd></div>
          </dl>
        </Reveal>
      </div>
    </section>
  )
}

function FeatureCard({ feature, index }) {
  const ref = useRef(null)
  // The glow follows the pointer through CSS variables, without re-rendering.
  const onPointerMove = (e) => {
    const card = ref.current
    if (!card) return
    const box = card.getBoundingClientRect()
    card.style.setProperty("--x", `${e.clientX - box.left}px`)
    card.style.setProperty("--y", `${e.clientY - box.top}px`)
  }
  return (
    <Reveal as="article" className="card" index={index}>
      <div className="card-inner" ref={ref} onPointerMove={onPointerMove}>
        <span className="card-tag" translate="no">{feature.tag}</span>
        <h3>{feature.title}</h3>
        <p>{feature.text}</p>
      </div>
    </Reveal>
  )
}

function Features() {
  return (
    <section className="section" id="features" aria-labelledby="features-title">
      <div className="wrap">
        <Reveal as="h2" id="features-title" className="section-title">Features</Reveal>
        <div className="cards">
          {FEATURES.map((f, i) => (
            <FeatureCard feature={f} index={i} key={f.title} />
          ))}
        </div>
      </div>
    </section>
  )
}

function Screens() {
  const [active, setActive] = useState(0)
  const go = (step) => setActive((i) => (i + step + SHOTS.length) % SHOTS.length)
  const onKeyDown = (e) => {
    if (e.key === "ArrowRight") { e.preventDefault(); go(1) }
    if (e.key === "ArrowLeft") { e.preventDefault(); go(-1) }
  }
  const touch = useRef(null)
  return (
    <section className="section screens" id="screens" aria-labelledby="screens-title">
      <div className="wrap">
        <Reveal as="h2" id="screens-title" className="section-title">Screenshots</Reveal>
        <Reveal
          className="stage"
          role="region"
          aria-roledescription="carousel"
          aria-label="Screenshots, use the arrow keys to change"
          tabIndex={0}
          onKeyDown={onKeyDown}
          onTouchStart={(e) => (touch.current = e.touches[0].clientX)}
          onTouchEnd={(e) => {
            if (touch.current === null) return
            const dx = e.changedTouches[0].clientX - touch.current
            if (Math.abs(dx) > 40) go(dx < 0 ? 1 : -1)
            touch.current = null
          }}
        >
          {SHOTS.map((s, i) => (
            <img
              key={s.src}
              src={s.src}
              alt={i === active ? s.alt : ""}
              aria-hidden={i !== active}
              className={i === active ? "slide is-active" : "slide"}
              width="1600"
              height="900"
              loading={i === 0 ? "eager" : "lazy"}
            />
          ))}
          <div className="stage-bar">
            <span className="stage-title" aria-live="polite">
              {SHOTS[active].title}
              <small>{active + 1} / {SHOTS.length}</small>
            </span>
            <div className="stage-controls">
              <button className="round" onClick={() => go(-1)} aria-label="Previous screenshot">‹</button>
              <button className="round" onClick={() => go(1)} aria-label="Next screenshot">›</button>
              <a className="round round-wide" href={SHOTS[active].src} target="_blank" rel="noreferrer">Full size</a>
            </div>
          </div>
        </Reveal>
        <div className="thumbs" role="group" aria-label="Choose a screenshot">
          {SHOTS.map((s, i) => (
            <button
              key={s.src}
              className={i === active ? "thumb is-active" : "thumb"}
              onClick={() => setActive(i)}
              aria-label={`Show ${s.title}`}
              aria-current={i === active}
            >
              <img src={s.src} alt="" width="320" height="180" loading="lazy" />
            </button>
          ))}
        </div>
        <p className="caption">Captured in OpenSLRR at 2560×1440 with the game data of SLRR{NB}2.2.1 MWM.</p>
      </div>
    </section>
  )
}

function Download() {
  return (
    <section className="section download" id="download" aria-labelledby="download-title">
      <div className="wrap">
        <Reveal as="h2" id="download-title" className="section-title">Download</Reveal>
        <div className="download-grid">
          <Reveal className="plate plate-primary" index={0}>
            <h3>Play</h3>
            <ol>
              <li>Install <b translate="no">Street Legal Racing: Redline 2.2.1 MWM</b>. OpenSLRR uses its data; none of it is distributed here.</li>
              <li>Build OpenSLRR with the build guide (there is no prebuilt release yet).</li>
              <li>Point it at your game folder and start it.</li>
            </ol>
            <a className="pill pill-primary" href={DOWNLOAD}>Open the build guide</a>
          </Reveal>
          <Reveal className="plate" index={1}>
            <h3>Build from source</h3>
            <ol>
              <li>Windows 10 or 11 x64, Visual Studio{NB}2022 &amp; CMake.</li>
              <li>The FMOD Studio API{NB}2.04 for Windows (not included).</li>
              <li>Clone the repository and follow the build guide.</li>
            </ol>
            <a className="pill" href={BUILD_GUIDE}>Open the build guide</a>
          </Reveal>
          <Reveal className="plate plate-wide" index={2}>
            <h3>Requirements</h3>
            <dl className="readout readout-row">
              <div><dt>OS</dt><dd>Windows 10 / 11, 64-bit</dd></div>
              <div><dt>GPU</dt><dd>Direct3D{NB}11</dd></div>
              <div><dt>Game</dt><dd>SLRR{NB}2.2.1 MWM</dd></div>
            </dl>
          </Reveal>
        </div>
      </div>
    </section>
  )
}

export default function App() {
  useInitialFragment()
  return (
    <>
      <a className="skip-link" href="#main">Skip to content</a>
      <Header />
      <main id="main">
        <Hero />
        <Stats />
        <Status />
        <Features />
        <Screens />
        <Download />
      </main>
      <Footer />
    </>
  )
}
