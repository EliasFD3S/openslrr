import { useEffect, useRef, useState } from "react"

export const prefersReducedMotion = () =>
  typeof window !== "undefined" &&
  window.matchMedia &&
  window.matchMedia("(prefers-reduced-motion: reduce)").matches

// True once the element has entered the viewport (it stays true).
export function useInView(options = { threshold: 0.2 }) {
  const ref = useRef(null)
  const [seen, setSeen] = useState(false)
  useEffect(() => {
    const node = ref.current
    if (!node || seen) return
    if (!("IntersectionObserver" in window)) {
      setSeen(true)
      return
    }
    const observer = new IntersectionObserver(([entry]) => {
      if (entry.isIntersecting) {
        setSeen(true)
        observer.disconnect()
      }
    }, options)
    observer.observe(node)
    return () => observer.disconnect()
  }, [seen])
  return [ref, seen]
}

// Counts from 0 to `target` over `duration` ms once `run` is true.
export function useCountUp(target, run, duration = 1400) {
  const [value, setValue] = useState(0)
  useEffect(() => {
    if (!run) return
    if (prefersReducedMotion()) {
      setValue(target)
      return
    }
    let frame
    const start = performance.now()
    const tick = (now) => {
      const t = Math.min((now - start) / duration, 1)
      const eased = 1 - Math.pow(1 - t, 3)
      setValue(target * eased)
      if (t < 1) frame = requestAnimationFrame(tick)
    }
    frame = requestAnimationFrame(tick)
    return () => cancelAnimationFrame(frame)
  }, [run, target, duration])
  return value
}

// True once the page has scrolled past `offset` pixels.
export function useScrolled(offset = 24) {
  const [scrolled, setScrolled] = useState(false)
  useEffect(() => {
    const onScroll = () => setScrolled(window.scrollY > offset)
    onScroll()
    window.addEventListener("scroll", onScroll, { passive: true })
    return () => window.removeEventListener("scroll", onScroll)
  }, [offset])
  return scrolled
}
