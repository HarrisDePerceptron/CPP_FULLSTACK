"use strict";

const byId = (id) => document.getElementById(id);

async function api(path, method = "GET", data, requireSession = true) {
    const options = { method };
    if (data !== undefined) {
        options.headers = { "Content-Type": "application/json" };
        options.body = JSON.stringify(data);
    }
    const response = await fetch(path, options);
    if (response.status === 401 && requireSession) {
        location.replace("/social/login");
        throw new Error("Your session expired. Sign in again.");
    }
    const body = await response.json();
    if (!response.ok) throw new Error(body.error?.message ?? "The request failed.");
    return body;
}

async function run(button, status, action) {
    if (button.disabled) return;
    button.disabled = true;
    status.textContent = "";
    try {
        await action();
    } catch (error) {
        status.textContent = error.message || "Try again.";
    } finally {
        button.disabled = false;
    }
}

function accountPage(form) {
    let signup = false;
    const submit = byId("account-submit");
    const mode = byId("account-mode");
    const status = byId("account-status");
    function setMode(value) {
        signup = value;
        byId("account-heading").textContent = signup ? "Create an account" : "Sign in";
        submit.textContent = signup ? "Create account" : "Sign in";
        mode.textContent = signup ? "Back to sign in" : "Create an account";
        form.elements.username.maxLength = signup ? 32 : 128;
        form.elements.password.minLength = signup ? 8 : 1;
        form.elements.password.autocomplete = signup ? "new-password" : "current-password";
        byId("signup-hint").hidden = !signup;
        form.elements.password.value = "";
        status.textContent = "";
    }
    mode.addEventListener("click", () => setMode(!signup));
    form.addEventListener("submit", (event) => {
        event.preventDefault();
        run(submit, status, async () => {
            mode.disabled = true;
            try {
                const credentials = Object.fromEntries(new FormData(form));
                await api(signup ? "/api/signup" : "/api/login", "POST", credentials, false);
                if (signup) {
                    setMode(false);
                    status.textContent = "Account created. Sign in to open the feed.";
                    form.elements.password.focus();
                } else {
                    location.replace("/social");
                }
            } finally {
                mode.disabled = false;
            }
        });
    });
}

function element(tag, classes, text = "") {
    const node = document.createElement(tag);
    node.className = classes;
    node.textContent = text;
    return node;
}

function message(item) {
    const block = element("div", "min-w-0");
    const time = element("time", "text-sm text-muted", new Date(item.created_at).toLocaleString());
    time.dateTime = item.created_at;
    block.append(element("p", "break-all font-semibold", item.author), time,
        element("p", "mt-2 whitespace-pre-wrap wrap-anywhere", item.text));
    return block;
}

function postCard(post) {
    const base = `/api/posts/${post.id}`;
    const card = element("article", "panel space-y-4");
    const status = element("p", "text-sm text-muted");
    status.setAttribute("role", "status");
    const like = element("button", "secondary");
    function updateLike(state) {
        post.liked = state.liked;
        like.textContent = `${state.liked ? "Unlike" : "Like"} (${state.like_count})`;
        like.setAttribute("aria-pressed", String(state.liked));
    }
    updateLike(post);
    like.addEventListener("click", () => run(like, status, async () => {
        const reply = await api(`${base}/like`, post.liked ? "DELETE" : "PUT");
        updateLike(reply.data[0]);
    }));

    const thread = element("details", "space-y-3");
    const summary = element("summary", "cursor-pointer font-semibold text-moss", `Comments (${post.comment_count})`);
    const comments = element("div", "space-y-3");
    const more = element("button", "secondary", "Load comments");
    const form = element("form", "space-y-2");
    const label = element("label", "label", "Add a comment");
    const input = element("textarea", "control resize-y");
    input.rows = 2;
    input.required = true;
    const send = element("button", "action", "Comment");
    label.append(input);
    form.append(label, send);
    let page = 1;
    const seen = new Set();
    function addComment(comment) {
        if (seen.has(comment.id)) return;
        seen.add(comment.id);
        comments.append(message(comment));
    }
    function load() {
        run(more, status, async () => {
            const reply = await api(`${base}/comments?page=${page}`);
            reply.data.forEach(addComment);
            page++;
            more.hidden = !reply.meta.pagination.has_more;
            more.textContent = "Load more comments";
        });
    }
    thread.addEventListener("toggle", () => { if (thread.open && page === 1) load(); });
    more.addEventListener("click", load);
    form.addEventListener("submit", (event) => {
        event.preventDefault();
        run(send, status, async () => {
            const text = input.value;
            const reply = await api(`${base}/comments`, "POST", { text });
            if (more.hidden) addComment(reply.data[0]);
            else status.textContent = "Comment posted. Load more comments to see it.";
            if (input.value === text) input.value = "";
            summary.textContent = `Comments (${++post.comment_count})`;
        });
    });

    thread.append(summary, comments, more, form);
    card.append(message(post), like, thread, status);
    return card;
}

async function feedPage(form) {
    const feed = byId("feed");
    const status = byId("feed-status");
    const more = byId("more-posts");
    const logout = byId("sign-out");
    const submit = form.querySelector("button");
    let page = 1;
    const seen = new Set();
    async function load() {
        const reply = await api(`/api/posts?page=${page}`);
        for (const post of reply.data) {
            if (seen.has(post.id)) continue;
            seen.add(post.id);
            feed.append(postCard(post));
        }
        status.textContent = seen.size ? "" : "No posts yet. Start the conversation.";
        page++;
        more.hidden = !reply.meta.pagination.has_more;
    }
    more.addEventListener("click", () => run(more, status, load));
    logout.addEventListener("click", () => run(logout, status, async () => {
        await api("/api/logout", "POST");
        location.replace("/social/login");
    }));
    form.addEventListener("submit", (event) => {
        event.preventDefault();
        run(submit, status, async () => {
            const text = form.elements.text.value;
            const reply = await api("/api/posts", "POST", { text });
            const post = reply.data[0];
            seen.add(post.id);
            feed.prepend(postCard(post));
            if (form.elements.text.value === text) form.reset();
        });
    });
    try {
        const identity = await api("/api/me");
        byId("viewer").textContent = `Signed in as ${identity.data[0].username}`;
        logout.disabled = false;
        await run(more, status, load);
        submit.disabled = false;
        if (page === 1) more.hidden = false;
    } catch (error) {
        status.textContent = error.message;
    }
}

const account = byId("account");
if (account) accountPage(account);
else feedPage(byId("compose"));
