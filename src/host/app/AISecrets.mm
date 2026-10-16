#include "AIModels.hpp"
#import <Foundation/Foundation.h>
#import <Security/Security.h>

namespace flows::app {
namespace {
NSMutableDictionary* query(const juce::String& credentialId) {
    return [@{(__bridge id)kSecClass:(__bridge id)kSecClassGenericPassword,
              (__bridge id)kSecAttrService:@"audio.flows.ai-models",
              (__bridge id)kSecAttrAccount:[NSString stringWithUTF8String:credentialId.toRawUTF8()]} mutableCopy];
}
void checkStatus(OSStatus status) {
    if(status!=errSecSuccess) throw std::runtime_error("macOS Keychain could not access the API key (status "+std::to_string(status)+"). Unlock Keychain and try again.");
}
}
AISecrets systemAISecrets() {
    return {
        [](const juce::String& credentialId) -> juce::String { @autoreleasepool {
            auto q=query(credentialId);q[(__bridge id)kSecReturnData]=@YES;q[(__bridge id)kSecMatchLimit]=(__bridge id)kSecMatchLimitOne;
            CFTypeRef result=nullptr;const auto status=SecItemCopyMatching((__bridge CFDictionaryRef)q,&result);[q release];
            if(status==errSecItemNotFound)return {};
            checkStatus(status);
            auto bytes=(CFDataRef)result;auto key=juce::String::fromUTF8((const char*)CFDataGetBytePtr(bytes),(int)CFDataGetLength(bytes));CFRelease(result);return key;
        }},
        [](const juce::String& credentialId,const juce::String& key) { @autoreleasepool {
            auto q=query(credentialId);q[(__bridge id)kSecValueData]=[NSData dataWithBytes:key.toRawUTF8() length:key.getNumBytesAsUTF8()];
            q[(__bridge id)kSecAttrLabel]=@"Flows AI model";
            const auto status=SecItemAdd((__bridge CFDictionaryRef)q,nullptr);[q release];checkStatus(status);
        }},
        [](const juce::String& credentialId) { @autoreleasepool {auto q=query(credentialId);const auto status=SecItemDelete((__bridge CFDictionaryRef)q);[q release];if(status!=errSecItemNotFound)checkStatus(status);}}
    };
}
}
