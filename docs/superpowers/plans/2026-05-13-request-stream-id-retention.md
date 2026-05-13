# Request Stream ID Retention and Cancellation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Retain the QUIC bidirectional stream IDs opened by `send_request_stream_and_wait` so `MoqtSession::close()` can reset them, correctly honouring draft-18's stream-lifetime semantics for PUBLISH_NAMESPACE and PUBLISH-track request streams.

**Architecture:** Add `reset_stream` to the `PublisherTransport` interface and implement it in `WebTransportClient` using `picoquic_reset_stream`. Extend `send_request_stream_and_wait` with an optional `out_stream_id` parameter. Store the namespace stream ID and a per-request-id map of track stream IDs in `MoqtSession` private state. Thread the map into the two free functions (`forward_published_tracks`, `publish_selected_tracks`) that open PUBLISH-track streams. Call `reset_stream` on all retained IDs before closing the connection.

**Tech Stack:** C++17, picoquic (`picoquic_reset_stream`), CMake in-source build, custom test harness in `tests/moqt_session_test.cpp`.

---

### Task 1: Add `reset_stream` to `PublisherTransport` and `MockTransport`

**Files:**
- Modify: `include/openmoq/publisher/transport/publisher_transport.h:57-74`
- Modify: `tests/moqt_session_test.cpp:41-143`

- [ ] **Step 1: Write the failing test**

Add this test function at the end of `tests/moqt_session_test.cpp`, just before the final `return ok ? 0 : 1;` in `main` (or alongside the other test sections). First find the `return ok ? 0 : 1;` line at the end of main:

```cpp
// In tests/moqt_session_test.cpp, add inside main() before the final return:
{
    MockTransport transport;
    transport.state_ = ConnectionState::kConnected;
    const TransportStatus rst = transport.reset_stream(42, 0);
    ok &= expect(rst.ok, "mock reset_stream should succeed");
    ok &= expect(transport.reset_calls.size() == 1, "expected one reset_stream call");
    ok &= expect(transport.reset_calls[0].first == 42, "expected stream_id 42");
    ok &= expect(transport.reset_calls[0].second == 0, "expected error_code 0");
}
```

- [ ] **Step 2: Add `reset_calls` recording and `reset_stream` override to `MockTransport`**

In `tests/moqt_session_test.cpp`, add to `MockTransport` (after line 129, before the data members):

```cpp
    TransportStatus reset_stream(std::uint64_t stream_id, std::uint64_t error_code) override {
        reset_calls.emplace_back(stream_id, error_code);
        return TransportStatus::success();
    }
```

And add the data member alongside the other members (after `last_close_code`):

```cpp
    std::vector<std::pair<std::uint64_t, std::uint64_t>> reset_calls;
```

- [ ] **Step 3: Run the test to verify it fails to compile**

```bash
cmake --build . --target openmoq-publisher-transport-tests 2>&1 | tail -10
```

Expected: compile error — `reset_stream` not declared in `PublisherTransport`.

- [ ] **Step 4: Add `reset_stream` to the `PublisherTransport` interface**

In `include/openmoq/publisher/transport/publisher_transport.h`, add after `read_stream`:

```cpp
    virtual TransportStatus reset_stream(std::uint64_t stream_id,
                                         std::uint64_t error_code) = 0;
```

The interface now looks like:

```cpp
    virtual TransportStatus open_stream(StreamDirection direction, std::uint64_t& stream_id) = 0;
    virtual TransportStatus write_stream(std::uint64_t stream_id,
                                         std::span<const std::uint8_t> bytes,
                                         bool fin) = 0;
    virtual TransportStatus read_stream(std::uint64_t stream_id,
                                        std::vector<std::uint8_t>& bytes,
                                        bool& fin,
                                        std::chrono::milliseconds timeout) = 0;
    virtual TransportStatus reset_stream(std::uint64_t stream_id,
                                         std::uint64_t error_code) = 0;
    virtual std::string connection_id() const = 0;
    virtual TransportStatus close(std::uint64_t application_error_code) = 0;
```

- [ ] **Step 5: Build transport tests**

```bash
cmake --build . --target openmoq-publisher-transport-tests 2>&1 | tail -5
```

Expected: compile error — `WebTransportClient` does not override `reset_stream`. That is expected; we will fix it in Task 2.

- [ ] **Step 6: Commit**

```bash
git add include/openmoq/publisher/transport/publisher_transport.h tests/moqt_session_test.cpp
git commit -m "Add reset_stream to PublisherTransport interface and MockTransport"
```

---

### Task 2: Implement `reset_stream` in `WebTransportClient`

**Files:**
- Modify: `include/openmoq/publisher/transport/webtransport_client.h:10-42`
- Modify: `src/transport/webtransport_client.cpp` (after `read_stream`, before `close`)

- [ ] **Step 1: Declare the override in the header**

In `include/openmoq/publisher/transport/webtransport_client.h`, add after the `read_stream` declaration (line 25):

```cpp
    TransportStatus reset_stream(std::uint64_t stream_id,
                                 std::uint64_t error_code) override;
```

- [ ] **Step 2: Implement `reset_stream` in the `.cpp`**

In `src/transport/webtransport_client.cpp`, add the following implementation between `read_stream` and `close` (around line 810):

```cpp
TransportStatus WebTransportClient::reset_stream(std::uint64_t stream_id,
                                                  std::uint64_t error_code) {
#ifndef OPENMOQ_HAS_PICOQUIC
    static_cast<void>(stream_id);
    static_cast<void>(error_code);
    return TransportStatus::failure("picoquic support is not enabled in this build");
#else
    if (impl_ == nullptr || impl_->cnx == nullptr) {
        return TransportStatus::failure("transport is not connected");
    }
    picoquic_reset_stream(impl_->cnx, stream_id, error_code);
    return TransportStatus::success();
#endif
}
```

- [ ] **Step 3: Build the full project to confirm no remaining override errors**

```bash
cmake --build . --target openmoq-publisher-transport-tests 2>&1 | tail -5
```

Expected: `[100%] Built target openmoq-publisher-transport-tests`

- [ ] **Step 4: Run the tests to confirm the new mock test passes**

```bash
./openmoq-publisher-transport-tests 2>/dev/null; echo "exit: $?"
```

Expected: `exit: 0`

- [ ] **Step 5: Commit**

```bash
git add include/openmoq/publisher/transport/webtransport_client.h src/transport/webtransport_client.cpp
git commit -m "Implement reset_stream in WebTransportClient using picoquic_reset_stream"
```

---

### Task 3: Add `out_stream_id` parameter to `send_request_stream_and_wait`

**Files:**
- Modify: `src/transport/moqt_session.cpp:781-920` (the function definition)

- [ ] **Step 1: Update the function signature**

In `src/transport/moqt_session.cpp`, change the signature of `send_request_stream_and_wait` at line 781 from:

```cpp
TransportStatus send_request_stream_and_wait(PublisherTransport& transport,
                                             openmoq::publisher::DraftVersion draft,
                                             std::span<const std::uint8_t> request_bytes,
                                             bool expect_publish_ack,
                                             PublishOk* publish_ok = nullptr) {
```

to:

```cpp
TransportStatus send_request_stream_and_wait(PublisherTransport& transport,
                                             openmoq::publisher::DraftVersion draft,
                                             std::span<const std::uint8_t> request_bytes,
                                             bool expect_publish_ack,
                                             PublishOk* publish_ok = nullptr,
                                             std::uint64_t* out_stream_id = nullptr) {
```

- [ ] **Step 2: Populate `out_stream_id` on every success path**

There are three `return TransportStatus::success()` calls inside `send_request_stream_and_wait`. Each must set `*out_stream_id` before returning. Add the helper just before the `while (true)` loop at line 803:

```cpp
    auto set_out_stream = [&]() {
        if (out_stream_id != nullptr) {
            *out_stream_id = request_stream_id;
        }
    };
```

Then apply `set_out_stream()` at the three success returns:

1. Line ~860 (PUBLISH_OK branch):
```cpp
                    set_out_stream();
                    return TransportStatus::success();
```

2. Line ~886 (timeout/no-queued-read in extra-response check):
```cpp
                        set_out_stream();
                        return TransportStatus::success();
```

3. Line ~901 (extra_fin with empty buffer):
```cpp
                    set_out_stream();
                    return TransportStatus::success();
```

Also find the success return at the bottom of the `if (fin)` block (around line 917-920) and add `set_out_stream()` there:

```cpp
        if (fin) {
            if (!buffered.empty()) {
                return TransportStatus::failure("trailing bytes after request response with fin");
            }
            set_out_stream();
            return TransportStatus::success();
        }
```

- [ ] **Step 3: Build and run tests to confirm no regressions**

```bash
cmake --build . --target openmoq-publisher-transport-tests 2>&1 | tail -3 && ./openmoq-publisher-transport-tests 2>/dev/null; echo "exit: $?"
```

Expected: build succeeds, `exit: 0`

- [ ] **Step 4: Commit**

```bash
git add src/transport/moqt_session.cpp
git commit -m "Add optional out_stream_id parameter to send_request_stream_and_wait"
```

---

### Task 4: Add private state to `MoqtSession`

**Files:**
- Modify: `include/openmoq/publisher/transport/moqt_session.h:40-58`

- [ ] **Step 1: Add `<map>` include and the two new members**

In `include/openmoq/publisher/transport/moqt_session.h`, add `<map>` to the includes (after `<optional>`):

```cpp
#include <map>
```

Then add the two new private members after `setup_complete_`:

```cpp
    bool setup_complete_ = false;
    std::uint64_t namespace_stream_id_ = 0;
    std::map<std::uint64_t, std::uint64_t> publish_stream_id_by_request_id_;
```

- [ ] **Step 2: Build to confirm no errors**

```bash
cmake --build . --target openmoq-publisher-transport-tests 2>&1 | tail -3
```

Expected: `[100%] Built target openmoq-publisher-transport-tests`

- [ ] **Step 3: Commit**

```bash
git add include/openmoq/publisher/transport/moqt_session.h
git commit -m "Add namespace_stream_id_ and publish_stream_id_by_request_id_ to MoqtSession"
```

---

### Task 5: Update `forward_published_tracks` to capture PUBLISH track stream IDs

**Files:**
- Modify: `src/transport/moqt_session.cpp:1811-1820` (signature), `~1847-1852` (capture), `~2315-2325` (call site)

- [ ] **Step 1: Add the map parameter to the `forward_published_tracks` signature**

Change the signature at line 1811 from:

```cpp
TransportStatus forward_published_tracks(PublisherTransport& transport,
                                         std::uint64_t control_stream_id,
                                         const openmoq::publisher::PublishPlan& plan,
                                         const LoopState& loop_state,
                                         std::span<const PublishedTrack> tracks,
                                         std::uint64_t peer_max_request_id,
                                         std::string_view track_namespace,
                                         bool paced,
                                         std::chrono::milliseconds subscriber_timeout,
                                         std::vector<std::uint8_t>& pending_control_bytes) {
```

to:

```cpp
TransportStatus forward_published_tracks(PublisherTransport& transport,
                                         std::uint64_t control_stream_id,
                                         const openmoq::publisher::PublishPlan& plan,
                                         const LoopState& loop_state,
                                         std::span<const PublishedTrack> tracks,
                                         std::uint64_t peer_max_request_id,
                                         std::string_view track_namespace,
                                         bool paced,
                                         std::chrono::milliseconds subscriber_timeout,
                                         std::vector<std::uint8_t>& pending_control_bytes,
                                         std::map<std::uint64_t, std::uint64_t>& publish_stream_ids) {
```

- [ ] **Step 2: Capture stream ID at the `send_request_stream_and_wait` call inside the function**

Around line 1847, change the call block from:

```cpp
        if (plan.draft.version == openmoq::publisher::DraftVersion::kDraft18) {
            PublishOk publish_ok;
            status = send_request_stream_and_wait(
                transport, plan.draft.version, encode_track_message(track_message), true, &publish_ok);
            if (status.ok) {
                publish_ok.request_id = next_request_id;
                publish_ok_by_request_id.insert_or_assign(next_request_id, publish_ok);
            }
```

to:

```cpp
        if (plan.draft.version == openmoq::publisher::DraftVersion::kDraft18) {
            PublishOk publish_ok;
            std::uint64_t track_stream_id = 0;
            status = send_request_stream_and_wait(
                transport, plan.draft.version, encode_track_message(track_message), true, &publish_ok,
                &track_stream_id);
            if (status.ok) {
                publish_ok.request_id = next_request_id;
                publish_ok_by_request_id.insert_or_assign(next_request_id, publish_ok);
                publish_stream_ids.emplace(next_request_id, track_stream_id);
            }
```

- [ ] **Step 3: Update the call site in `MoqtSession::publish()` (around line 2315)**

Change:

```cpp
        return forward_published_tracks(
            transport_,
            control_stream_id_,
            plan,
            loop_state,
            tracks,
            peer_max_request_id_,
            track_namespace_,
            paced_,
            subscriber_timeout_,
            pending_control_bytes_);
```

to:

```cpp
        return forward_published_tracks(
            transport_,
            control_stream_id_,
            plan,
            loop_state,
            tracks,
            peer_max_request_id_,
            track_namespace_,
            paced_,
            subscriber_timeout_,
            pending_control_bytes_,
            publish_stream_id_by_request_id_);
```

- [ ] **Step 4: Build and run tests**

```bash
cmake --build . --target openmoq-publisher-transport-tests 2>&1 | tail -3 && ./openmoq-publisher-transport-tests 2>/dev/null; echo "exit: $?"
```

Expected: build succeeds, `exit: 0`

- [ ] **Step 5: Commit**

```bash
git add src/transport/moqt_session.cpp
git commit -m "Thread publish_stream_ids out of forward_published_tracks into MoqtSession state"
```

---

### Task 6: Update `publish_selected_tracks` to capture PUBLISH track stream IDs

**Files:**
- Modify: `src/transport/moqt_session.cpp:2011-2021` (signature), `~2051-2056` (capture), `~2338-2348` (call site)

- [ ] **Step 1: Add the map parameter to `publish_selected_tracks` signature**

Change the signature at line 2011 from:

```cpp
TransportStatus publish_selected_tracks(PublisherTransport& transport,
                                        std::uint64_t control_stream_id,
                                        const openmoq::publisher::PublishPlan& plan,
                                        const LoopState& loop_state,
                                        std::span<const PublishedTrack> tracks,
                                        std::uint64_t peer_max_request_id,
                                        std::string_view track_namespace,
                                        bool paced,
                                        std::vector<std::uint8_t>& pending_control_bytes,
                                        std::map<std::uint64_t, DormantPublishedTrack>* dormant_published_tracks = nullptr,
                                        std::map<std::uint64_t, std::uint64_t>* request_id_by_track_alias = nullptr) {
```

to:

```cpp
TransportStatus publish_selected_tracks(PublisherTransport& transport,
                                        std::uint64_t control_stream_id,
                                        const openmoq::publisher::PublishPlan& plan,
                                        const LoopState& loop_state,
                                        std::span<const PublishedTrack> tracks,
                                        std::uint64_t peer_max_request_id,
                                        std::string_view track_namespace,
                                        bool paced,
                                        std::vector<std::uint8_t>& pending_control_bytes,
                                        std::map<std::uint64_t, std::uint64_t>& publish_stream_ids,
                                        std::map<std::uint64_t, DormantPublishedTrack>* dormant_published_tracks = nullptr,
                                        std::map<std::uint64_t, std::uint64_t>* request_id_by_track_alias = nullptr) {
```

- [ ] **Step 2: Capture stream ID at the `send_request_stream_and_wait` call inside the function**

Around line 2051, change:

```cpp
        if (plan.draft.version == openmoq::publisher::DraftVersion::kDraft18) {
            PublishOk publish_ok;
            status = send_request_stream_and_wait(
                transport, plan.draft.version, encode_track_message(track_message), true, &publish_ok);
            if (status.ok) {
                publish_ok.request_id = next_request_id;
                publish_ok_by_request_id.insert_or_assign(next_request_id, publish_ok);
            }
```

to:

```cpp
        if (plan.draft.version == openmoq::publisher::DraftVersion::kDraft18) {
            PublishOk publish_ok;
            std::uint64_t track_stream_id = 0;
            status = send_request_stream_and_wait(
                transport, plan.draft.version, encode_track_message(track_message), true, &publish_ok,
                &track_stream_id);
            if (status.ok) {
                publish_ok.request_id = next_request_id;
                publish_ok_by_request_id.insert_or_assign(next_request_id, publish_ok);
                publish_stream_ids.emplace(next_request_id, track_stream_id);
            }
```

- [ ] **Step 3: Update the call site in `MoqtSession::publish()` (around line 2338)**

Change:

```cpp
            status = publish_selected_tracks(transport_,
                                             control_stream_id_,
                                             plan,
                                             loop_state,
                                             selected_tracks,
                                             peer_max_request_id_,
                                             track_namespace_,
                                             paced_,
                                             pending_control_bytes_,
                                             &dormant_published_tracks,
                                             &request_id_by_track_alias);
```

to:

```cpp
            status = publish_selected_tracks(transport_,
                                             control_stream_id_,
                                             plan,
                                             loop_state,
                                             selected_tracks,
                                             peer_max_request_id_,
                                             track_namespace_,
                                             paced_,
                                             pending_control_bytes_,
                                             publish_stream_id_by_request_id_,
                                             &dormant_published_tracks,
                                             &request_id_by_track_alias);
```

- [ ] **Step 4: Build and run tests**

```bash
cmake --build . --target openmoq-publisher-transport-tests 2>&1 | tail -3 && ./openmoq-publisher-transport-tests 2>/dev/null; echo "exit: $?"
```

Expected: build succeeds, `exit: 0`

- [ ] **Step 5: Commit**

```bash
git add src/transport/moqt_session.cpp
git commit -m "Thread publish_stream_ids out of publish_selected_tracks into MoqtSession state"
```

---

### Task 7: Capture PUBLISH_NAMESPACE stream IDs at the two call sites

**Files:**
- Modify: `src/transport/moqt_session.cpp:~2293-2295` (publish path), `~2454-2456` (publish_live path)

- [ ] **Step 1: Capture namespace stream ID in `MoqtSession::publish()`**

Around line 2293, change:

```cpp
    if (plan.draft.version == openmoq::publisher::DraftVersion::kDraft18) {
        status = send_request_stream_and_wait(
            transport_, plan.draft.version, encode_namespace_message(namespace_message), false, nullptr);
```

to:

```cpp
    if (plan.draft.version == openmoq::publisher::DraftVersion::kDraft18) {
        status = send_request_stream_and_wait(
            transport_, plan.draft.version, encode_namespace_message(namespace_message), false, nullptr,
            &namespace_stream_id_);
```

- [ ] **Step 2: Capture namespace stream ID in `MoqtSession::publish_live()`**

Around line 2454, change:

```cpp
    if (draft_version == openmoq::publisher::DraftVersion::kDraft18) {
        status = send_request_stream_and_wait(
            transport_, draft_version, encode_namespace_message(namespace_message), false, nullptr);
```

to:

```cpp
    if (draft_version == openmoq::publisher::DraftVersion::kDraft18) {
        status = send_request_stream_and_wait(
            transport_, draft_version, encode_namespace_message(namespace_message), false, nullptr,
            &namespace_stream_id_);
```

- [ ] **Step 3: Build and run tests**

```bash
cmake --build . --target openmoq-publisher-transport-tests 2>&1 | tail -3 && ./openmoq-publisher-transport-tests 2>/dev/null; echo "exit: $?"
```

Expected: build succeeds, `exit: 0`

- [ ] **Step 4: Commit**

```bash
git add src/transport/moqt_session.cpp
git commit -m "Capture PUBLISH_NAMESPACE stream ID into namespace_stream_id_ on draft-18"
```

---

### Task 8: Update `MoqtSession::close()` to reset retained streams

**Files:**
- Modify: `src/transport/moqt_session.cpp:2867-2871`

- [ ] **Step 1: Update `MoqtSession::close()`**

Change the current implementation at line 2867 from:

```cpp
TransportStatus MoqtSession::close(std::uint64_t application_error_code) {
    control_stream_open_ = false;
    control_stream_id_ = 0;
    return transport_.close(application_error_code);
}
```

to:

```cpp
TransportStatus MoqtSession::close(std::uint64_t application_error_code) {
    if (namespace_stream_id_ != 0) {
        transport_.reset_stream(namespace_stream_id_, 0x0);
        namespace_stream_id_ = 0;
    }
    for (auto& [request_id, stream_id] : publish_stream_id_by_request_id_) {
        transport_.reset_stream(stream_id, 0x0);
    }
    publish_stream_id_by_request_id_.clear();
    control_stream_open_ = false;
    control_stream_id_ = 0;
    return transport_.close(application_error_code);
}
```

- [ ] **Step 2: Build and run tests**

```bash
cmake --build . --target openmoq-publisher-transport-tests 2>&1 | tail -3 && ./openmoq-publisher-transport-tests 2>/dev/null; echo "exit: $?"
```

Expected: build succeeds, `exit: 0`

- [ ] **Step 3: Commit**

```bash
git add src/transport/moqt_session.cpp
git commit -m "Reset retained draft-18 request stream IDs in MoqtSession::close()"
```

---

### Task 9: Write tests for stream ID retention and close-time reset

**Files:**
- Modify: `tests/moqt_session_test.cpp`

These tests verify the behaviour end-to-end using the MockTransport. Add them inside `main()` alongside the existing draft-18 publish test block (around line 1511).

- [ ] **Step 1: Write a test verifying namespace stream ID is retained after `publish()`**

Add after the existing draft-18 block (after line 1511):

```cpp
    {
        // After a successful draft-18 publish, the namespace stream ID is stored.
        // Stream IDs assigned by MockTransport: 0=control, 4=PUBLISH_NAMESPACE, 8=first PUBLISH, 12=second PUBLISH
        MockTransport d18_retain_transport;
        d18_retain_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft18,
            .max_request_id = 0,
        }));
        d18_retain_transport.reads[4].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        d18_retain_transport.reads[8].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft18, 2));
        d18_retain_transport.reads[12].push_back(encode_publish_namespace_ok_message(DraftVersion::kDraft18, 4));

        MoqtSession d18_retain_session(d18_retain_transport, std::string(kTestTrackNamespace), true);
        TransportStatus st = d18_retain_session.connect(endpoint, tls);
        ok &= expect(st.ok, "draft-18 retain: connect should succeed");

        const PublishPlan d18_plan =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft18), source_bytes);
        st = d18_retain_session.publish(d18_plan);
        ok &= expect(st.ok, "draft-18 retain: publish should succeed");

        // close() should trigger reset_stream for the namespace stream (4)
        // and each PUBLISH track stream (8, 12)
        st = d18_retain_session.close();
        ok &= expect(st.ok, "draft-18 retain: close should succeed");

        const auto& resets = d18_retain_transport.reset_calls;
        ok &= expect(resets.size() == 3, "expected 3 reset_stream calls on close (1 namespace + 2 track)");

        std::set<std::uint64_t> reset_stream_ids;
        for (const auto& [sid, ec] : resets) {
            reset_stream_ids.insert(sid);
            ok &= expect(ec == 0, "expected error_code 0 for all resets");
        }
        ok &= expect(reset_stream_ids.count(4) == 1, "expected namespace stream 4 to be reset");
        ok &= expect(reset_stream_ids.count(8) == 1, "expected track stream 8 to be reset");
        ok &= expect(reset_stream_ids.count(12) == 1, "expected track stream 12 to be reset");
    }
```

Also add `#include <set>` near the top of the test file if not already present. Check:

```bash
grep "<set>" tests/moqt_session_test.cpp
```

If missing, add it with the other includes.

- [ ] **Step 2: Write a test verifying double-close is safe (no double-reset)**

Add after the previous block:

```cpp
    {
        // Calling close() twice should not re-reset already-cleared stream IDs.
        MockTransport d18_double_close_transport;
        d18_double_close_transport.reads[0].push_back(encode_server_setup_message({
            .draft = DraftVersion::kDraft18,
            .max_request_id = 0,
        }));
        d18_double_close_transport.reads[4].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 0));
        d18_double_close_transport.reads[8].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 2));
        d18_double_close_transport.reads[12].push_back(
            encode_publish_namespace_ok_message(DraftVersion::kDraft18, 4));

        MoqtSession d18_dc_session(d18_double_close_transport, std::string(kTestTrackNamespace), true);
        d18_dc_session.connect(endpoint, tls);
        const PublishPlan d18_plan2 =
            materialize_publish_plan(make_span_backed_plan(DraftVersion::kDraft18), source_bytes);
        d18_dc_session.publish(d18_plan2);

        d18_dc_session.close();
        const std::size_t after_first_close = d18_double_close_transport.reset_calls.size();
        d18_dc_session.close();
        ok &= expect(d18_double_close_transport.reset_calls.size() == after_first_close,
                     "second close() should not re-reset already-cleared stream IDs");
    }
```

- [ ] **Step 3: Build and run the tests**

```bash
cmake --build . --target openmoq-publisher-transport-tests 2>&1 | tail -3 && ./openmoq-publisher-transport-tests 2>/dev/null; echo "exit: $?"
```

Expected: build succeeds, `exit: 0`

- [ ] **Step 4: Commit**

```bash
git add tests/moqt_session_test.cpp
git commit -m "Add tests for draft-18 request stream ID retention and close-time reset"
```

---

### Task 10: Full build verification

- [ ] **Step 1: Build all test targets**

```bash
cmake --build . 2>&1 | tail -5
```

Expected: all targets build cleanly.

- [ ] **Step 2: Run CTest**

```bash
ctest --output-on-failure 2>&1 | tail -20
```

Expected: all tests pass.

- [ ] **Step 3: Confirm no regressions in control-message tests**

```bash
./openmoq-publisher-control-message-tests 2>/dev/null; echo "exit: $?"
./openmoq-publisher-cli-tests 2>/dev/null; echo "exit: $?"
```

Expected: both exit 0.
