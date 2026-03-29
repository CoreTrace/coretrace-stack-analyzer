// SPDX-License-Identifier: Apache-2.0
struct CreateInfo
{
    int tag;
};

extern void populateDebugMessengerCreateInfo(CreateInfo& out, unsigned int callback);

static const bool enableValidationLayers = false;

void fp_create_info_compile_time_disabled(void)
{
    if (!enableValidationLayers)
        return;

    CreateInfo createInfo;
    populateDebugMessengerCreateInfo(createInfo, 0u);
}

// not contains: local variable 'createInfo' is never initialized
