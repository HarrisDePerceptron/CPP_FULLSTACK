"use strict";

const $ = (id) => document.getElementById(id);
let token = null;
let busy = false;

function setSession(accessToken, username) {
    token = accessToken;
    $("session-status").textContent = token ? `Signed in as ${username}` : "Signed out";
    for (const id of ["whoami", "logout", "upload-button"]) {
        $(id).disabled = !token;
    }
}

function showResult(status, body, downloadUrl) {
    $("response-status").textContent = status;
    $("response-body").textContent = typeof body === "string" ? body : JSON.stringify(body, null, 2);
    $("download-row").classList.toggle("hidden", !downloadUrl);
    if (downloadUrl) $("download").href = downloadUrl;
    else $("download").removeAttribute("href");
}

async function request(path, options = {}, protectedRoute = false, onSuccess) {
    if (busy) return;
    busy = true;
    showResult(`${options.method ?? "GET"} ${path}`, "Waiting for server…");
    try {
        const headers = new Headers(options.headers);
        if (protectedRoute && token) headers.set("Authorization", `Bearer ${token}`);
        const response = await fetch(path, { ...options, headers });
        const type = response.headers.get("content-type") ?? "";
        const body = type.includes("application/json") ? await response.json() : await response.text();
        if (protectedRoute && response.status === 401) setSession(null);
        const status = `${response.status} ${response.statusText}`;
        if (response.ok && onSuccess) {
            onSuccess(status, body);
        } else {
            showResult(status, body);
        }
    } catch {
        showResult("Request failed", "Could not complete the request.");
    } finally {
        busy = false;
    }
}

$("greet").addEventListener("click", () => request("/hello"));
$("fetch-content").addEventListener("click", () => request("/api/content"));
$("whoami").addEventListener("click", () => request("/api/me", {}, true));
$("logout").addEventListener("click", () => {
    if (busy) return;
    setSession(null);
    showResult("Signed out", "The page has cleared the session.");
});

$("login").addEventListener("submit", (event) => {
    event.preventDefault();
    if (busy) return;
    const form = event.currentTarget;
    const { username, password } = Object.fromEntries(new FormData(form));
    request("/api/login", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username, password }),
    }, false, (status, body) => {
        setSession(body.access_token, username);
        showResult(status, { token_type: body.token_type, expires_in: body.expires_in });
    });
    form.elements.password.value = "";
});

$("upload").addEventListener("submit", (event) => {
    event.preventDefault();
    if (busy || !token) return;
    const data = new FormData(event.currentTarget);
    if (data.get("file").size > 10 * 1024 * 1024) {
        showResult("File too large", "Choose a file up to 10 MiB.");
        return;
    }
    request("/api/uploads", { method: "POST", body: data }, true, (status, body) => {
        const publicUrl = /^\/public\/uploads\/[a-f0-9]{32}\.(pdf|png|jpg|gif|webp)$/.test(body.url)
            ? body.url : undefined;
        showResult(status, body, publicUrl);
    });
});
