import React from "react"
import { createRoot } from "react-dom/client"
import { Footer, Header, useInitialFragment } from "./SiteLayout.jsx"
import "./styles.css"
import "./launcher.css"

function Launcher() {
  useInitialFragment()
  return (
    <>
      <a className="skip-link" href="#main">Skip to content</a>
      <Header launcher />
      <main id="main" tabIndex={-1}>
        <section className="hero launcher-hero" aria-labelledby="launcher-title">
          <div className="hero-content">
            <span className="card-tag">Desktop companion</span>
            <h1 id="launcher-title" className="hero-title">OpenSLRR Launcher</h1>
            <p className="hero-line">
              Manage <strong>OpenSLRR engine versions</strong> and start Street Legal Racing: Redline
              with your own game files.
            </p>
            <p className="launcher-notice">There is no public launcher installer available here.</p>
            <div className="actions">
              <a className="pill pill-primary" href="#how-it-works">How it works</a>
              <a className="pill" href="../docs/#/build">Engine build guide</a>
            </div>
          </div>
        </section>
        <section className="section launcher-details" id="how-it-works" aria-labelledby="workflow-title">
          <div className="wrap">
            <h2 className="section-title" id="workflow-title">How it works</h2>
            <div className="download-grid">
              <article className="plate plate-primary">
                <h3>First run</h3>
                <ol>
                  <li>In <b>Settings</b>, select your own SLRR 2.2.1 MWM folder. The launcher copies the files it needs without changing your original installation.</li>
                  <li>In <b>Versions</b>, install a compatible engine release when available, or import a trusted, compatible local engine ZIP.</li>
                  <li>In <b>Play</b>, start the selected version. Each engine version has its own game folder and saves.</li>
                </ol>
              </article>
              <article className="plate">
                <h3>What you need</h3>
                <ul>
                  <li>Windows 10 or 11 x64 and a Direct3D 11 GPU. Installing the engine and playing through the launcher currently require Windows x64.</li>
                  <li>Your own copy of <b>Street Legal Racing: Redline 2.2.1 MWM</b>. No original game data is included.</li>
                </ul>
                <p>To run OpenSLRR without the launcher, use the public engine build guide.</p>
                <a className="pill" href="../docs/#/build">Build the engine</a>
              </article>
            </div>
          </div>
        </section>
      </main>
      <Footer launcher />
    </>
  )
}

createRoot(document.getElementById("root")).render(
  <React.StrictMode>
    <Launcher />
  </React.StrictMode>,
)
