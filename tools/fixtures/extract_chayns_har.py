#!/usr/bin/env python3
"""Extract deterministic, allowlisted Chayns contracts from local HAR files.

HAR files can contain live credentials and personal data.  This script never
copies request headers and reconstructs response bodies from an explicit field
allowlist.  Unknown fields are dropped.  The committed output therefore uses
only synthetic identifiers and content while retaining the wire shape and
behaviour-relevant enum/capability values.
"""

import argparse
import json
from pathlib import Path
import re
import sys
from urllib.parse import parse_qsl, urlencode, urlsplit


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_HAR_DIR = ROOT / "har"
DEFAULT_OUTPUT_DIR = ROOT / "src/test/fixtures/chayns"
FIXED_TIME = "2026-01-01T00:00:00.000Z"


def load_entries(path):
    with path.open(encoding="utf-8") as stream:
        payload = json.load(stream)
    entries = payload.get("log", {}).get("entries")
    if not isinstance(entries, list):
        raise ValueError("{} has no HAR log.entries array".format(path))
    return entries


def entry_path(entry):
    return urlsplit(entry["request"]["url"]).path


def find_entry(entries, method, path_pattern, status=None, predicate=None):
    pattern = re.compile(path_pattern)
    for entry in entries:
        request = entry.get("request", {})
        response = entry.get("response", {})
        if request.get("method") != method or not pattern.fullmatch(entry_path(entry)):
            continue
        if status is not None and response.get("status") != status:
            continue
        if predicate is not None and not predicate(entry):
            continue
        return entry
    raise ValueError("missing HAR entry: {} {} status={}".format(method, path_pattern, status))


def json_body(container, required=True):
    text = container.get("text", "")
    if not text:
        if required:
            raise ValueError("expected JSON body")
        return None
    return json.loads(text)


def request_json(entry):
    return json_body(entry.get("request", {}).get("postData", {}))


def response_json(entry, required=True):
    return json_body(entry.get("response", {}).get("content", {}), required)


def normalized_path(entry):
    parsed = urlsplit(entry["request"]["url"])
    path = re.sub(
        r"(/intercom-backend/v2/thread/)[^/]+(/message)$", r"\1{threadId}\2", parsed.path
    )
    if not parsed.query:
        return path
    query = []
    for key, value in parse_qsl(parsed.query, keep_blank_values=True):
        if key in {"afterDate", "beforeDate"}:
            value = "{timestamp}"
        if key in {"forceCreate", "take", "viewMode", "afterDate", "beforeDate"}:
            query.append((key, value))
    return path + ("?" + urlencode(query, safe="{}") if query else "")


def safe_request(entry, body=None):
    request = {
        "method": entry["request"]["method"],
        "path": normalized_path(entry),
    }
    if body is not None:
        request["headers"] = {"content-type": "application/json"}
        request["body"] = body
    return request


def safe_response(entry, body_marker=False, body=None):
    response = {"status": int(entry["response"]["status"])}
    if body_marker:
        response["headers"] = {"content-type": "application/json"}
        response["body"] = body
    return response


def safe_message(source, index, author_role="agent", message_id=None):
    type_id = int(source.get("typeId", 0))
    if author_role == "user":
        text = "synthetic user question"
    elif type_id == 1:
        text = "synthetic final answer"
    elif type_id == 18:
        text = "synthetic reasoning delta"
    else:
        text = "synthetic message"
    return {
        "id": message_id or "<message-{}>".format(index),
        "threadId": "<thread-1>",
        "author": {"id": "<{}-author>".format(author_role)},
        "typeId": type_id,
        "creationTime": "2026-01-01T00:00:{:02d}.000Z".format(index),
        "modifiedTime": "2026-01-01T00:00:{:02d}.000Z".format(index),
        "text": text,
    }


def safe_thread_request(source, pro):
    result = {
        "members": [
            {"isAdmin": True, "personId": "<user-person>"},
            {"personId": "<agent-person>"},
        ],
        "nerMode": source.get("nerMode", "None"),
        "priority": int(source.get("priority", 0)),
        "typeId": int(source["typeId"]),
        "messages": [{"text": "synthetic user question"}],
    }
    if pro:
        if "workspaceUacId" not in source:
            raise ValueError("pro thread fixture is missing workspaceUacId")
        result["workspaceUacId"] = 9001
    return result


def safe_thread_response(source, pro):
    result = {
        "id": "<thread-1>",
        "typeId": int(source["typeId"]),
        "creationTime": FIXED_TIME,
        "modifiedTime": FIXED_TIME,
        "members": [
            {
                "personId": "<user-person>",
                "tobitId": 1001,
                "isAdmin": True,
                "id": "<user-author>",
                "isCreator": True,
                "isAgent": False,
            },
            {
                "personId": "<agent-person>",
                "tobitId": 1002,
                "isAdmin": False,
                "id": "<agent-author>",
                "isCreator": False,
                "isAgent": True,
            },
        ],
        "anonymizationForAI": bool(source.get("anonymizationForAI", False)),
        "nerMode": source.get("nerMode", "None"),
        "messages": [
            {
                "id": "<request-message>",
                "threadId": "<thread-1>",
                "author": {"id": "<user-author>"},
                "typeId": int(source.get("messages", [{}])[0].get("typeId", 1)),
                "creationTime": FIXED_TIME,
                "modifiedTime": FIXED_TIME,
                "text": "synthetic user question",
            }
        ],
    }
    if pro:
        result["workspaceUacId"] = 9001
    return result


def safe_model(source, index, requires_pro):
    allowed_bool = [
        "needSidekickPro",
        "canHandleImages",
        "canHandleFunctionCalling",
        "canHandleGoogleSearch",
        "canUseThinking",
    ]
    result = {
        "personId": "<model-person-{}>".format(index),
        "showName": "fixture-{}-model".format("pro" if requires_pro else "free"),
        "usedModel": int(source.get("usedModel", index)),
        "tobitId": 2000 + index,
        "supportedMimeTypes": [
            value
            for value in source.get("supportedMimeTypes", [])
            if isinstance(value, str) and re.fullmatch(r"[-+.*a-zA-Z0-9]+/[-+.*a-zA-Z0-9]+", value)
        ],
        "skills": {
            key: int(value)
            for key, value in source.get("skills", {}).items()
            if isinstance(key, str) and isinstance(value, int)
        },
        "developer": "fixture-developer",
        "developerCountry": "ZZ",
        "hostingProvider": "fixture-host",
        "hostingCountry": "ZZ",
        "knowledge": FIXED_TIME,
        "costIndicator": int(source.get("costIndicator", 0)),
    }
    for key in allowed_bool:
        if key in source:
            result[key] = bool(source[key])
    # The selector and fixture assertion must agree even if an upstream item
    # omits the field to represent the free tier.
    if requires_pro:
        result["needSidekickPro"] = True
    else:
        result.pop("needSidekickPro", None)
    return result


def envelope(name, source_name, entry, request_body=None, response_body_marker=False, response_body=None):
    return {
        "schemaVersion": 1,
        "name": name,
        "source": source_name,
        "sanitization": "explicit-allowlist-v1",
        "request": safe_request(entry, request_body),
        "response": safe_response(entry, response_body_marker, response_body),
    }


def extract(har_dir):
    free_name = "sidekick.ki.har"
    pro_name = "mein.sidekick.ki.har"
    delete_name = "delete.sidekick.ki.har"
    model_name = "login-mein.sidekick.ki.har"
    free_entries = load_entries(har_dir / free_name)
    pro_entries = load_entries(har_dir / pro_name)
    delete_entries = load_entries(har_dir / delete_name)
    model_entries = load_entries(har_dir / model_name)

    thread_pattern = r"/intercom-backend/v2/thread"
    message_pattern = r"/intercom-backend/v2/thread/[^/]+/message"

    free = find_entry(free_entries, "POST", thread_pattern, 201)
    pro = find_entry(pro_entries, "POST", thread_pattern, 201)
    followup = find_entry(free_entries, "POST", message_pattern, 201)
    poll_empty = find_entry(
        free_entries,
        "GET",
        message_pattern,
        204,
        lambda item: "take=1000" in urlsplit(item["request"]["url"]).query,
    )
    poll_messages = find_entry(
        free_entries,
        "GET",
        message_pattern,
        200,
        lambda item: len(response_json(item)) >= 3,
    )
    read = find_entry(free_entries, "PATCH", r"/intercom-backend/v2/thread/read", 200)
    delete = find_entry(
        delete_entries, "DELETE", r"/intercom-backend/v2/thread/member/delete", 200
    )
    models = find_entry(
        model_entries, "GET", r"/chayns-ai-chatbot/nativeModelChatbot", 200
    )

    followup_source = request_json(followup)
    followup_request = {
        "cursorPosition": len("synthetic follow-up question"),
        "text": "synthetic follow-up question",
    }
    if "images" in followup_source:
        followup_request["images"] = [{"url": "https://example.invalid/image.png"}]

    poll_source = response_json(poll_messages)
    raw_user_author = poll_source[0].get("author", {}).get("id")
    safe_poll = []
    for index, item in enumerate(poll_source):
        role = "user" if item.get("author", {}).get("id") == raw_user_author else "agent"
        safe_poll.append(
            safe_message(
                item,
                index + 1,
                role,
                "<request-message>" if index == 0 else None,
            )
        )

    catalog = response_json(models)
    free_model = next(item for item in catalog if not bool(item.get("needSidekickPro", False)))
    pro_model = next(item for item in catalog if bool(item.get("needSidekickPro", False)))

    outputs = {
        "thread-create-free.json": envelope(
            "thread-create-free",
            free_name,
            free,
            safe_thread_request(request_json(free), False),
            True,
            safe_thread_response(response_json(free), False),
        ),
        "thread-create-pro.json": envelope(
            "thread-create-pro",
            pro_name,
            pro,
            safe_thread_request(request_json(pro), True),
            True,
            safe_thread_response(response_json(pro), True),
        ),
        "message-create.json": envelope(
            "message-create",
            free_name,
            followup,
            followup_request,
            True,
            safe_message(
                response_json(followup),
                1,
                "user",
                "<followup-request-message>",
            ),
        ),
        "poll-empty.json": envelope("poll-empty", free_name, poll_empty),
        "poll-messages.json": envelope(
            "poll-messages", free_name, poll_messages, None, True, safe_poll
        ),
        "thread-read.json": envelope(
            "thread-read",
            free_name,
            read,
            {"isRead": True, "threadIds": ["<thread-1>"]},
        ),
        "thread-delete.json": envelope(
            "thread-delete",
            delete_name,
            delete,
            {"threadIds": ["<thread-1>"], "personId": "<user-person>"},
        ),
        "model-catalog.json": envelope(
            "model-catalog",
            model_name,
            models,
            None,
            True,
            [safe_model(free_model, 1, False), safe_model(pro_model, 2, True)],
        ),
    }
    return outputs


def safety_errors(outputs):
    errors = []
    forbidden_fragments = [
        "authorization",
        "cookie",
        "bearer ",
        "access_token",
        "refresh_token",
        "password",
        "cube.tobit.cloud",
    ]
    for name, payload in outputs.items():
        serialized = json.dumps(payload, ensure_ascii=False).lower()
        for fragment in forbidden_fragments:
            if fragment in serialized:
                errors.append("{} contains forbidden fragment {!r}".format(name, fragment))
        if re.search(r"[A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{20,}", serialized):
            errors.append("{} contains a JWT-like value".format(name))
        if re.search(r"[\w.+-]+@[\w.-]+\.[A-Za-z]{2,}", serialized):
            errors.append("{} contains an email-like value".format(name))
    return errors


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--har-dir", default=str(DEFAULT_HAR_DIR))
    parser.add_argument("--output-dir", default=str(DEFAULT_OUTPUT_DIR))
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail if committed fixtures differ; do not write files",
    )
    args = parser.parse_args()
    har_dir = Path(args.har_dir)
    output_dir = Path(args.output_dir)
    try:
        outputs = extract(har_dir)
    except (OSError, ValueError, KeyError, StopIteration, json.JSONDecodeError) as error:
        print("fixture extraction failed: {}".format(error), file=sys.stderr)
        return 2
    errors = safety_errors(outputs)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 2

    rendered = {
        name: json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
        for name, payload in outputs.items()
    }
    if args.check:
        differences = []
        for name, expected in rendered.items():
            path = output_dir / name
            if not path.is_file() or path.read_text(encoding="utf-8") != expected:
                differences.append(name)
        if differences:
            print("fixture drift: " + ", ".join(differences), file=sys.stderr)
            return 1
        print("Chayns fixture extraction/safety: PASS ({} files)".format(len(rendered)))
        return 0

    output_dir.mkdir(parents=True, exist_ok=True)
    for name, content in rendered.items():
        (output_dir / name).write_text(content, encoding="utf-8")
    print("wrote {} sanitized Chayns fixtures".format(len(rendered)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
