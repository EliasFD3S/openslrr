(function () {
  const data = window.SLRR_CATALOG || { natives: [], added: [] };
  const rows = data.natives.concat(data.added);
  const labels = {
    host: "In host",
    partial: "Partial",
    noop: "No-op",
    absent: "Not in host",
    added: "OpenSLRR"
  };
  const list = document.getElementById("list");
  const classes = document.getElementById("classes");
  const counts = document.getElementById("counts");
  const query = document.getElementById("q");
  let active = "All";

  function text(value) {
    return String(value)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;");
  }

  const names = ["All"].concat(
    Array.from(new Set(rows.map((row) => row.class))).sort()
  );
  classes.innerHTML = names
    .map((name) => {
      const label = name === "All" ? "All natives" : name;
      return `<button type="button" data-class="${text(name)}">${text(label)}</button>`;
    })
    .join("");

  function signature(row) {
    if (!row.ret) return `${row.name}${row.params}`;
    return `${row.ret} ${row.name}(${row.params || ""})`;
  }

  function render() {
    const needle = query.value.trim().toLowerCase();
    const shown = rows.filter((row) => {
      if (active !== "All" && row.class !== active) return false;
      if (!needle) return true;
      return `${row.class} ${row.name} ${row.params} ${row.note}`.toLowerCase().includes(needle);
    });
    const groups = new Map();
    for (const row of shown) {
      if (!groups.has(row.class)) groups.set(row.class, []);
      groups.get(row.class).push(row);
    }
    const html = [];
    for (const [name, items] of groups) {
      html.push(`<div class="group">${text(name)}</div>`);
      for (const row of items) {
        const note = row.note ? `<div class="note">${text(row.note)}</div>` : "";
        html.push(
          `<article class="native"><div class="badge ${row.status}">${labels[row.status]}</div><div><div class="sig">${text(signature(row))}</div>${note}</div></article>`
        );
      }
    }
    list.innerHTML = html.join("") || `<article class="native"><div class="note">No native matches.</div></article>`;
    counts.textContent = `${shown.length} / ${rows.length}`;
    for (const button of classes.querySelectorAll("button")) {
      button.classList.toggle("on", button.dataset.class === active);
    }
  }

  classes.addEventListener("click", (event) => {
    const button = event.target.closest("button");
    if (!button) return;
    active = button.dataset.class;
    render();
  });
  query.addEventListener("input", render);
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      query.value = "";
      active = "All";
      render();
    }
  });
  render();
})();
