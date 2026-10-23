#include "app/PublishConfig.h"
#include "publish/MoqxrPublisher.h"

#include <chrono>
#include <iostream>

int main() {
    bool ok = true;
    const auto check = [&ok](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ok = false;
        }
    };

    check(moq2ts::PublishConfig{}.draftVersion == 22, "new sessions default to MOQT draft 22");
    check(moq2ts::MoqxrPublisher::supportsDraft(22), "draft 22 is accepted");
    check(moq2ts::MoqxrPublisher::supportsDraft(21), "draft 21 is accepted");
    check(moq2ts::MoqxrPublisher::supportsDraft(18), "draft 18 remains accepted");
    check(!moq2ts::MoqxrPublisher::supportsDraft(20), "unsupported draft 20 is rejected");
    check(!moq2ts::MoqxrPublisher::supportsDraft(23), "unknown draft 23 is rejected");

    // A live source keeps waiting for subscribers: moqxr ends a publish that
    // waited subscriber_timeout for one, and a live publisher must outlast
    // any time without a subscriber. A file keeps its bounded wait.
    using moq2ts::MoqxrPublisher;
    check(MoqxrPublisher::subscriberTimeout(true) >= std::chrono::hours(24 * 365),
          "a live source waits for subscribers without a practical bound");
    check(MoqxrPublisher::subscriberTimeout(false) == std::chrono::seconds(120),
          "a file source keeps its 120 s wait for a subscriber");
    // moqxr adds the timeout to steady_clock::now(), in nanoseconds: the sum
    // must not overflow.
    check(MoqxrPublisher::subscriberTimeout(true) <
              std::chrono::duration_cast<std::chrono::seconds>(std::chrono::nanoseconds::max()) / 4,
          "the live wait does not overflow a steady_clock deadline");

    // A publish that moqxr ends without an error is a failure for a live
    // source, unless moq2ts asked for it: a stop, or the end of the source.
    // Arguments: live source, running, publisher stopped, source ended.
    const QString early = MoqxrPublisher::unexpectedEndReason(true, true, false, false);
    check(!early.isEmpty(), "a live publish that ends on its own has a reason");
    check(early.contains(QStringLiteral("live source")), "the reason says the live source was still running");
    check(MoqxrPublisher::unexpectedEndReason(true, false, false, false).isEmpty(), "a stop is no unexpected end");
    // SIGINT/SIGTERM on the --config path: MoqxrPublisher::stop() ends the
    // publish before the pipeline clears running.
    check(MoqxrPublisher::unexpectedEndReason(true, true, true, false).isEmpty(),
          "an interrupt that stops the publisher first is no unexpected end");
    check(MoqxrPublisher::unexpectedEndReason(true, true, false, true).isEmpty(),
          "the end of the source is no unexpected end");
    check(MoqxrPublisher::unexpectedEndReason(false, true, false, false).isEmpty(), "a file publish keeps its behaviour");
    return ok ? 0 : 1;
}
