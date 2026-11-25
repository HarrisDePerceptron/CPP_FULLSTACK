"use strict";

(() => {
    const storageKey = "app-theme";
    const root = document.documentElement;

    function apply(value) {
        root.dataset.theme = value === "light" ? "light" : "dark";
    }

    try {
        apply(localStorage.getItem(storageKey));
    } catch {
        apply("dark");
    }

    document.addEventListener("DOMContentLoaded", () => {
        const switches = document.querySelectorAll("[data-theme-switch]");
        function updateSwitches() {
            for (const control of switches) {
                control.setAttribute("aria-checked", String(root.dataset.theme === "dark"));
                control.hidden = false;
            }
        }
        for (const control of switches) {
            control.addEventListener("click", () => {
                apply(root.dataset.theme === "dark" ? "light" : "dark");
                updateSwitches();
                try {
                    localStorage.setItem(storageKey, root.dataset.theme);
                } catch {
                    // The selected theme still applies to this page.
                }
            });
        }
        window.addEventListener("storage", (event) => {
            if (event.key === storageKey || event.key === null) {
                apply(event.newValue);
                updateSwitches();
            }
        });
        updateSwitches();
    });
})();
