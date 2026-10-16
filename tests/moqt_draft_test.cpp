#include "app/PublishConfig.h"
#include "publish/MoqxrPublisher.h"

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
    return ok ? 0 : 1;
}
