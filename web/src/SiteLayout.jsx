import { useEffect, useLayoutEffect, useRef, useState } from "react"
import { useScrolled } from "./hooks.js"

export const REPO = "https://github.com/EliasFD3S/openslrr"

// On a direct link, the browser may look for the fragment before React mounts it.
export function useInitialFragment() {
  useLayoutEffect(() => {
    const id = window.location.hash.slice(1)
    if (id) document.getElementById(id)?.scrollIntoView({ behavior: "instant" })
  }, [])
}

export function Header({ launcher = false }) {
  const [open, setOpen] = useState(false)
  const menu = useRef(null)
  const base = launcher ? "../" : "./"
  const scrolled = useScrolled(40)
  useEffect(() => {
    if (!open) return
    const onKey = (e) => {
      if (e.key !== "Escape") return
      setOpen(false)
      menu.current?.focus()
    }
    window.addEventListener("keydown", onKey)
    return () => window.removeEventListener("keydown", onKey)
  }, [open])
  const close = () => setOpen(false)
  return (
    <header className={`topbar ${launcher || scrolled || open ? "is-solid" : ""}`}>
      <div className="topbar-inner">
        <a className="brand" href={launcher ? base : "#top"} onClick={close} aria-label={launcher ? "OpenSLRR engine home" : "OpenSLRR, back to top"}>
          <img src={`${base}img/logo.png`} alt="" width="96" height="46" />
        </a>
        <button
          ref={menu}
          className="menu-toggle"
          aria-expanded={open}
          aria-controls="site-nav"
          onClick={() => setOpen(!open)}
        >
          {open ? "Close" : "Menu"}
        </button>
        <nav id="site-nav" className={open ? "nav is-open" : "nav"} aria-label="Main">
          <a href={launcher ? `${base}#features` : "#features"} onClick={close}>Features</a>
          <a href={launcher ? `${base}#screens` : "#screens"} onClick={close}>Screenshots</a>
          <a href={launcher ? `${base}#download` : "#download"} onClick={close}>Download</a>
          <a href={launcher ? "./" : "./launcher/"} aria-current={launcher ? "page" : undefined} onClick={close}>Launcher</a>
          <a href={`${base}docs/`}>Documentation</a>
          <a href={REPO} target="_blank" rel="noreferrer">GitHub</a>
        </nav>
      </div>
    </header>
  )
}

export function Footer({ launcher = false }) {
  const docs = launcher ? "../docs/" : "./docs/"
  return (
    <footer className="bottombar">
      <div className="wrap footer-inner">
        <nav className="footer-nav" aria-label="Footer">
          {launcher ? <a href="../">Engine home</a> : <a href="./launcher/">Launcher</a>}
          <a href={docs}>Documentation</a>
          <a href={`${docs}#/changelog`}>Changelog</a>
          <a href={REPO} target="_blank" rel="noreferrer">GitHub</a>
          <a href={`${REPO}/issues`} target="_blank" rel="noreferrer">Report an issue</a>
        </nav>
        <p>
          OpenSLRR is a fan project released under the MIT licence. It is not affiliated with or endorsed by the
          publishers or developers of Street Legal Racing: Redline, and it needs your own copy of the game.
        </p>
      </div>
    </footer>
  )
}
