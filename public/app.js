"use strict";

const $ = (id) => document.getElementById(id);
const uploadForm = $("upload");
const uploadFile = uploadForm.elements.file;
let signedIn = false;
let busy = false;

function updateUploadButton() {
    $("upload-button").disabled = !signedIn || uploadFile.files.length === 0 || busy;
}

function setSession(username) {
    signedIn = Boolean(username);
    $("session-status").textContent = signedIn ? `Signed in as ${username}` : "Signed out";
    for (const id of ["whoami", "logout"]) {
        $(id).disabled = !signedIn;
    }
    updateUploadButton();
}

function showResult(status, body, downloadUrl) {
    $("response-status").textContent = status;
    $("response-body").textContent = typeof body === "string" ? body : JSON.stringify(body, null, 2);
    $("download-row").classList.toggle("hidden", !downloadUrl);
    if (downloadUrl) $("download").href = downloadUrl;
    else $("download").removeAttribute("href");
}

async function request(path, options = {}, protectedRoute = false, onSuccess, restoringSession = false) {
    if (busy) return;
    busy = true;
    updateUploadButton();
    if (!restoringSession) showResult(`${options.method ?? "GET"} ${path}`, "Waiting for server…");
    try {
        const response = await fetch(path, options);
        if (protectedRoute && response.status === 401) {
            setSession(null);
            if (restoringSession) return;
        }
        const type = response.headers.get("content-type") ?? "";
        const body = type.includes("application/json") ? await response.json() : await response.text();
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
        updateUploadButton();
    }
}

$("check-status").addEventListener("click", () => request("/status"));
$("fetch-content").addEventListener("click", () => request("/api/content"));
$("whoami").addEventListener("click", () => request("/api/me", {}, true));
$("logout").addEventListener("click", () => {
    request("/api/logout", { method: "POST" }, false, (status, body) => {
        setSession(null);
        showResult(status, body);
    });
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
        setSession(username);
        const { token_type, expires_in } = body.data[0];
        showResult(status, { ...body, data: [{ token_type, expires_in }] });
    });
    form.elements.password.value = "";
});

uploadFile.addEventListener("change", updateUploadButton);
updateUploadButton();

uploadForm.addEventListener("submit", (event) => {
    event.preventDefault();
    if (busy || !signedIn || uploadFile.files.length === 0) return;
    const data = new FormData(event.currentTarget);
    if (data.get("file").size > 10 * 1024 * 1024) {
        showResult("File too large", "Choose a file up to 10 MiB.");
        return;
    }
    request("/api/uploads", { method: "POST", body: data }, true, (status, body) => {
        const publicUrl = /^\/public\/uploads\/[a-f0-9]{32}\.(pdf|png|jpg|gif|webp)$/.test(body.data[0].url)
            ? body.data[0].url : undefined;
        showResult(status, body, publicUrl);
    });
});

request("/api/me", {}, true, (_status, body) => setSession(body.data[0].username), true);
