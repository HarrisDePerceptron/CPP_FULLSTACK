"use strict";

const byId = (id) => document.getElementById(id);

async function api(path, method = "GET", data, requireSession = true, signal) {
    const options = { method, signal };
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
                    location.replace("/");
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

function initials(username) {
    return [...username].slice(0, 2).join("").toUpperCase();
}

const timestamp = new Intl.DateTimeFormat(undefined, { dateStyle: "medium", timeStyle: "short" });

function message(item) {
    const block = element("div", "flex items-start gap-3");
    const avatar = element("span", "avatar", initials(item.author));
    avatar.setAttribute("aria-hidden", "true");
    const content = element("div", "min-w-0 flex-1");
    const heading = element("div", "flex flex-wrap items-baseline gap-x-2");
    const time = element("time", "text-xs text-muted", timestamp.format(new Date(item.created_at)));
    time.dateTime = item.created_at;
    heading.append(element("span", "wrap-anywhere font-semibold", item.author), time);
    content.append(heading, element("p", "mt-1.5 whitespace-pre-wrap wrap-anywhere leading-relaxed", item.text));
    block.append(avatar, content);
    return block;
}

function composer(form, input, submit, counter, status, limit, send) {
    let ready = false;
    let pending = false;
    const length = () => [...input.value.replace(/^[ \t\n\r\f\v]+|[ \t\n\r\f\v]+$/g, "")].length;
    function refresh() {
        const size = length();
        counter.textContent = `${size} / ${limit}`;
        counter.dataset.invalid = String(size > limit);
        input.setAttribute("aria-invalid", String(size > limit));
        submit.disabled = !ready || pending || size === 0 || size > limit;
    }
    input.addEventListener("input", refresh);
    form.addEventListener("submit", async (event) => {
        event.preventDefault();
        if (submit.disabled) return;
        const draft = input.value;
        pending = true;
        refresh();
        status.textContent = "";
        try {
            await send(draft);
            if (input.value === draft) input.value = "";
        } catch (error) {
            status.textContent = error.message || "Could not send. Try again.";
        } finally {
            pending = false;
            refresh();
        }
    });
    refresh();
    return () => {
        ready = true;
        refresh();
    };
}

function liveStatus(classes) {
    const status = element("p", classes);
    status.setAttribute("role", "status");
    return status;
}

function timelineAction(pathData) {
    const button = element("button", "timeline-action");
    button.type = "button";
    const svgNamespace = "http://www.w3.org/2000/svg";
    const icon = document.createElementNS(svgNamespace, "svg");
    icon.setAttribute("class", "size-[17px] shrink-0");
    icon.setAttribute("viewBox", "0 0 24 24");
    icon.setAttribute("fill", "none");
    icon.setAttribute("stroke", "currentColor");
    icon.setAttribute("stroke-width", "1.7");
    icon.setAttribute("stroke-linecap", "round");
    icon.setAttribute("stroke-linejoin", "round");
    icon.setAttribute("aria-hidden", "true");
    const path = document.createElementNS(svgNamespace, "path");
    path.setAttribute("d", pathData);
    icon.append(path);
    const count = element("span", "");
    count.setAttribute("aria-hidden", "true");
    button.append(icon, count);
    return { button, count };
}

let nextCard = 0;

function postCard(post) {
    const cardId = ++nextCard;
    const base = `/api/posts/${post.id}`;
    const card = element("article", "space-y-1 border-b border-rule/55 px-4 pb-2 pt-4 last:border-b-0 sm:px-7");
    card.dataset.postId = String(post.id);
    const actions = element("div", "ml-11 flex flex-wrap items-center gap-3");
    const status = liveStatus("text-sm text-danger empty:hidden");
    const { button: like, count: likes } = timelineAction(
        "M12 20.5 4.6 13a4.6 4.6 0 0 1 6.5-6.5l.9.9.9-.9a4.6 4.6 0 0 1 6.5 6.5Z");
    function updateLike(state) {
        post.liked = state.liked;
        likes.textContent = String(state.like_count);
        like.setAttribute("aria-label", `${state.liked ? "Unlike" : "Like"} (${state.like_count})`);
        like.setAttribute("aria-pressed", String(state.liked));
    }
    updateLike(post);
    like.addEventListener("click", () => run(like, status, async () => {
        const reply = await api(`${base}/like`, post.liked ? "DELETE" : "PUT");
        syncPost("post-like", reply.data[0]);
    }));

    const { button: toggle, count: replies } = timelineAction(
        "M20.5 11.3a8.5 7.3 0 0 1-8.5 7.3 9.6 9.6 0 0 1-2.8-.4L5 20l.9-3.2a6.8 6.8 0 0 1-2.4-5.5A8.5 7.3 0 0 1 12 4a8.5 7.3 0 0 1 8.5 7.3Z");
    function updateReplies() {
        replies.textContent = String(post.comment_count);
        toggle.setAttribute("aria-label", `Replies (${post.comment_count})`);
    }
    updateReplies();
    const thread = element("div", "ml-4 space-y-3 rounded-xl bg-paper/50 p-3 sm:ml-13 sm:p-4");
    thread.id = `replies-${cardId}`;
    thread.hidden = true;
    toggle.setAttribute("aria-controls", thread.id);
    toggle.setAttribute("aria-expanded", "false");
    const stack = element("div", "ml-4 space-y-2 sm:ml-13");
    const comments = element("div", "space-y-4 border-l border-rule/55 pl-3");
    comments.id = `comments-${cardId}`;
    const threadStatus = liveStatus("text-sm text-muted empty:hidden");
    const more = element("button", "feed-text-action underline underline-offset-4", "See more comments");
    more.type = "button";
    more.setAttribute("aria-controls", comments.id);
    more.hidden = !post.latest_comment || post.comment_count < 2;
    const form = element("form", "space-y-2 pt-1");
    const label = element("label", "label", "Write a reply");
    const input = element("textarea", "feed-input mt-1 bg-field/75 text-sm");
    input.rows = 2;
    input.required = true;
    const counter = element("span", "composer-counter");
    counter.id = `reply-limit-${cardId}`;
    input.setAttribute("aria-describedby", counter.id);
    const send = element("button", "action rounded-full", "Reply");
    const replyStatus = liveStatus("text-sm text-danger empty:hidden");
    const footer = element("div", "flex items-center justify-between gap-3");
    label.append(input);
    footer.append(counter, send);
    form.append(label, footer, replyStatus);
    let cursor = post.latest_comment;
    const visibleComments = new Set();
    function showComment(item, newest = false) {
        if (visibleComments.has(item.id)) return false;
        visibleComments.add(item.id);
        const node = message(item);
        if (newest) comments.prepend(node);
        else comments.append(node);
        stack.hidden = false;
        return true;
    }
    stack.hidden = true;
    if (cursor) showComment(cursor);

    function syncPost(type, value) {
        for (const match of document.querySelectorAll(`[data-post-id="${post.id}"]`)) {
            match.dispatchEvent(new CustomEvent(type, { detail: value }));
        }
    }
    card.addEventListener("post-like", ({ detail }) => updateLike(detail));
    card.addEventListener("post-reply", ({ detail: item }) => {
        if (!showComment(item, true)) return;
        cursor ??= item;
        post.comment_count++;
        updateReplies();
    });

    async function load() {
        if (!cursor) return;
        await run(more, threadStatus, async () => {
            threadStatus.textContent = "Loading comments…";
            const query = new URLSearchParams({ before_id: cursor.id, before_time: cursor.created_at });
            const reply = await api(`${base}/comments?${query}`);
            for (const item of reply.data) showComment(item);
            cursor = reply.data.at(-1) ?? cursor;
            if (!reply.meta.pagination.has_more && document.activeElement === more) {
                toggle.focus();
            }
            more.hidden = !reply.meta.pagination.has_more;
            threadStatus.textContent = "";
        });
    }
    toggle.addEventListener("click", () => {
        thread.hidden = !thread.hidden;
        toggle.setAttribute("aria-expanded", String(!thread.hidden));
        if (!thread.hidden) input.focus();
    });
    more.addEventListener("click", load);
    const enableReply = composer(form, input, send, counter, replyStatus, 500, async (text) => {
        const reply = await api(`${base}/comments`, "POST", { text });
        syncPost("post-reply", reply.data[0]);
    });
    enableReply();
    actions.append(toggle, like);
    stack.append(comments, more, threadStatus);
    thread.append(form);
    card.append(message(post), actions, status, stack, thread);
    return card;
}

async function feedPage(form) {
    const feed = byId("feed");
    const status = byId("feed-status");
    const more = byId("more-posts");
    const pagination = byId("feed-pagination");
    const logout = byId("sign-out");
    const submit = form.querySelector("button");
    let page = 1;
    let signedIn = false;
    const seen = new Set();
    const enablePost = composer(form, form.elements.text, submit, byId("post-limit"), byId("post-feedback"), 280, async (text) => {
        const reply = await api("/api/posts", "POST", { text });
        const post = reply.data[0];
        if (!seen.has(post.id)) {
            seen.add(post.id);
            feed.prepend(postCard(post));
        }
        if (page > 1 && pagination.hidden) status.textContent = "";
    });

    async function load() {
        await run(more, status, async () => {
            status.textContent = page === 1 ? "Loading feed…" : "Loading more posts…";
            more.textContent = "Loading…";
            try {
                if (!signedIn) {
                    const identity = await api("/api/me");
                    const username = identity.data[0].username;
                    byId("viewer").textContent = username;
                    const avatar = byId("viewer-avatar");
                    avatar.textContent = initials(username);
                    avatar.hidden = false;
                    byId("composer-avatar").textContent = avatar.textContent;
                    logout.disabled = false;
                    signedIn = true;
                    enablePost();
                }
                const reply = await api(`/api/posts?page=${page}`);
                for (const post of reply.data) {
                    if (seen.has(post.id)) continue;
                    seen.add(post.id);
                    feed.append(postCard(post));
                }
                page++;
                more.textContent = "Load more posts";
                pagination.hidden = !reply.meta.pagination.has_more;
                status.textContent = seen.size ? "" : "No posts yet. Start the conversation.";
            } catch (error) {
                more.textContent = "Retry feed";
                pagination.hidden = false;
                throw error;
            }
        });
    }

    more.addEventListener("click", load);
    logout.addEventListener("click", () => run(logout, byId("account-feedback"), async () => {
        await api("/api/logout", "POST");
        location.replace("/social/login");
    }));
    await load();
}

function accountMenu() {
    const button = byId("account-menu-button");
    const menu = byId("account-menu");
    function setOpen(open) {
        menu.classList.toggle("hidden", !open);
        menu.classList.toggle("flex", open);
        button.setAttribute("aria-expanded", String(open));
    }
    button.addEventListener("click", () => setOpen(button.getAttribute("aria-expanded") === "false"));
    matchMedia("(min-width: 64rem)").addEventListener("change", () => setOpen(false));
    document.addEventListener("click", (event) => {
        if (!menu.contains(event.target) && !button.contains(event.target)) setOpen(false);
    });
    document.addEventListener("keydown", (event) => {
        if (event.key === "Escape" && button.getAttribute("aria-expanded") === "true") {
            setOpen(false);
            button.focus();
        }
    });
}

function searchPage() {
    const search = byId("search");
    const input = byId("search-query");
    const clear = byId("search-clear");
    const dropdown = byId("search-results");
    const status = byId("search-status");
    const matches = byId("search-matches");
    const timeline = byId("timeline");
    const selection = byId("selection");
    const title = byId("selection-title");
    const detail = byId("selection-posts");
    const feedback = byId("selection-status");
    const more = byId("selection-more");
    const back = byId("search-back");
    let timer;
    let pendingSearch;
    let pendingSelection;
    let lastQuery;
    let loadSelection;

    function dismiss() {
        clearTimeout(timer);
        pendingSearch?.abort();
        dropdown.hidden = true;
    }

    function open(item, people) {
        dismiss();
        pendingSelection?.abort();
        const request = new AbortController();
        pendingSelection = request;
        timeline.hidden = true;
        selection.hidden = false;
        back.hidden = false;
        title.textContent = people ? `Posts by ${item.username}` : "Post";
        title.focus();
        detail.replaceChildren();
        more.hidden = true;
        more.disabled = false;
        let page = 1;
        const loaded = new Set();
        loadSelection = async () => {
            if (more.disabled) return;
            more.disabled = true;
            feedback.textContent = "Loading…";
            try {
                const path = people ? `/api/users/${item.id}/posts?page=${page}` : `/api/posts/${item.id}`;
                const response = await api(path, "GET", undefined, true, request.signal);
                if (request.signal.aborted) return;
                for (const post of response.data) {
                    if (loaded.has(post.id)) continue;
                    loaded.add(post.id);
                    detail.append(postCard(post));
                }
                feedback.textContent = loaded.size ? "" : "No posts yet.";
                page++;
                more.textContent = "Load more posts";
                more.hidden = !people || !response.meta.pagination.has_more;
            } catch (error) {
                if (request.signal.aborted) return;
                feedback.textContent = error.message || "Could not load posts.";
                more.textContent = "Retry";
                more.hidden = false;
            } finally {
                if (!request.signal.aborted) more.disabled = false;
            }
        };
        loadSelection();
    }

    function addGroup(label, items, people) {
        const group = element("section", "space-y-1 p-1");
        group.append(element("h2", "px-2 text-xs font-semibold text-muted", label));
        if (!items.length) {
            group.append(element("p", "px-2 py-1 text-sm text-muted", `No matching ${label.toLowerCase()}.`));
        }
        for (const item of items) {
            const button = element("button", "flex w-full items-center gap-2 rounded-lg px-2 py-2 text-left text-sm hover:bg-hover");
            button.type = "button";
            if (people) {
                const avatar = element("span", "avatar", initials(item.username));
                avatar.setAttribute("aria-hidden", "true");
                button.append(avatar, element("span", "wrap-anywhere font-semibold", item.username));
            } else {
                const copy = element("span", "min-w-0");
                copy.append(element("span", "block wrap-anywhere font-semibold", item.author),
                    element("span", "line-clamp-2 wrap-anywhere text-muted", item.text));
                button.append(copy);
            }
            button.addEventListener("click", () => open(item, people));
            group.append(button);
        }
        matches.append(group);
    }

    async function showResults() {
        clearTimeout(timer);
        const query = input.value.trim();
        if (!query) return;
        dropdown.hidden = false;
        if (lastQuery === query) return;
        pendingSearch?.abort();
        const request = new AbortController();
        pendingSearch = request;
        status.textContent = "Searching…";
        matches.replaceChildren();
        try {
            const response = await api(`/api/search?${new URLSearchParams({ q: query })}`, "GET", undefined, true, request.signal);
            if (request.signal.aborted || input.value.trim() !== query) return;
            const { posts, users } = response.data[0];
            lastQuery = query;
            status.textContent = posts.length || users.length ? "" : "No matches.";
            addGroup("Posts", posts, false);
            addGroup("People", users, true);
        } catch (error) {
            if (!request.signal.aborted) status.textContent = error.message || "Search failed. Press Enter to retry.";
        }
    }

    function resetQuery() {
        dismiss();
        lastQuery = undefined;
        matches.replaceChildren();
        clear.hidden = !input.value;
        if (input.value.trim()) timer = setTimeout(showResults, 250);
    }

    input.addEventListener("input", resetQuery);
    clear.addEventListener("click", () => {
        input.value = "";
        resetQuery();
        input.focus();
    });
    byId("search-form").addEventListener("submit", (event) => {
        event.preventDefault();
        showResults();
    });
    search.addEventListener("keydown", (event) => {
        if (event.key === "Escape" && (!dropdown.hidden || input.value)) {
            event.preventDefault();
            dismiss();
            input.focus();
        }
        if (!["ArrowDown", "ArrowUp"].includes(event.key) || dropdown.hidden) return;
        const buttons = [...matches.querySelectorAll("button")];
        const current = buttons.indexOf(document.activeElement);
        const next = event.key === "ArrowDown" ? buttons[current + 1] : buttons[current < 0 ? buttons.length - 1 : current - 1];
        if (next) {
            event.preventDefault();
            next.focus();
        }
    });
    document.addEventListener("click", (event) => {
        if (!search.contains(event.target)) dismiss();
    });
    more.addEventListener("click", () => loadSelection?.());
    back.addEventListener("click", () => {
        pendingSelection?.abort();
        selection.hidden = true;
        back.hidden = true;
        detail.replaceChildren();
        timeline.hidden = false;
        input.focus();
        showResults();
    });
}

const account = byId("account");
if (account) accountPage(account);
else {
    accountMenu();
    searchPage();
    feedPage(byId("compose"));
}
