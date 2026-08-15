// SPDX-License-Identifier: GPL-3.0-or-later

#include "firewall_access.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <netfw.h>
#include <shellapi.h>
#include <wrl/client.h>

#include "jamlink/diagnostics/session_log.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace jamlink::desktop {
namespace {

using Microsoft::WRL::ComPtr;

// Named so a repair can find every earlier attempt, including ones left behind
// by an update that moved the executable.
constexpr wchar_t ruleName[] = L"JamLink";

class ComScope final {
public:
    ComScope() noexcept {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        initialised_ = SUCCEEDED(result);
        usable_ = initialised_ || result == RPC_E_CHANGED_MODE;
    }
    ~ComScope() {
        if (initialised_) {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    [[nodiscard]] bool usable() const noexcept { return usable_; }

private:
    bool initialised_{false};
    bool usable_{false};
};

[[nodiscard]] QString executablePath() {
    return QDir::toNativeSeparators(
        QFileInfo(QCoreApplication::applicationFilePath()).absoluteFilePath());
}

[[nodiscard]] QString profileName(long profiles) {
    if ((profiles & NET_FW_PROFILE2_PUBLIC) != 0) {
        return QStringLiteral("Public");
    }
    if ((profiles & NET_FW_PROFILE2_PRIVATE) != 0) {
        return QStringLiteral("Private");
    }
    if ((profiles & NET_FW_PROFILE2_DOMAIN) != 0) {
        return QStringLiteral("Domain");
    }
    return QStringLiteral("Unknown");
}

[[nodiscard]] bool firewallOnFor(INetFwPolicy2& policy, long profiles) {
    const auto enabledFor = [&policy](NET_FW_PROFILE_TYPE2 profile) {
        VARIANT_BOOL enabled = VARIANT_FALSE;
        return SUCCEEDED(policy.get_FirewallEnabled(profile, &enabled))
            && enabled != VARIANT_FALSE;
    };
    if ((profiles & NET_FW_PROFILE2_PUBLIC) != 0) {
        return enabledFor(NET_FW_PROFILE2_PUBLIC);
    }
    if ((profiles & NET_FW_PROFILE2_PRIVATE) != 0) {
        return enabledFor(NET_FW_PROFILE2_PRIVATE);
    }
    if ((profiles & NET_FW_PROFILE2_DOMAIN) != 0) {
        return enabledFor(NET_FW_PROFILE2_DOMAIN);
    }
    return false;
}

} // namespace

FirewallAccess queryFirewallAccess() {
    FirewallAccess access;
    ComScope com;
    if (!com.usable()) {
        return access;
    }

    ComPtr<INetFwPolicy2> policy;
    if (FAILED(CoCreateInstance(
            __uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&policy)))
        || !policy) {
        return access;
    }

    long activeProfiles = 0;
    static_cast<void>(policy->get_CurrentProfileTypes(&activeProfiles));
    access.activeProfile = profileName(activeProfiles);

    if (!firewallOnFor(*policy.Get(), activeProfiles)) {
        access.state = FirewallRuleState::NotEnforced;
        return access;
    }

    ComPtr<INetFwRules> rules;
    if (FAILED(policy->get_Rules(&rules)) || !rules) {
        return access;
    }
    ComPtr<IUnknown> enumerator;
    if (FAILED(rules->get__NewEnum(&enumerator)) || !enumerator) {
        return access;
    }
    ComPtr<IEnumVARIANT> variants;
    if (FAILED(enumerator.As(&variants)) || !variants) {
        return access;
    }

    const QString wanted = executablePath();
    bool allowed = false;
    VARIANT item{};
    ULONG fetched = 0U;
    while (variants->Next(1U, &item, &fetched) == S_OK && fetched == 1U) {
        ComPtr<INetFwRule> rule;
        if (item.vt == VT_DISPATCH && item.pdispVal != nullptr
            && SUCCEEDED(item.pdispVal->QueryInterface(IID_PPV_ARGS(&rule))) && rule) {
            BSTR name = nullptr;
            BSTR application = nullptr;
            NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_IN;
            NET_FW_ACTION action = NET_FW_ACTION_BLOCK;
            VARIANT_BOOL enabled = VARIANT_FALSE;
            LONG protocol = 0;
            if (SUCCEEDED(rule->get_Name(&name)) && name != nullptr) {
                if (_wcsicmp(name, ruleName) == 0) {
                    ++access.matchingRules;
                    static_cast<void>(rule->get_ApplicationName(&application));
                    static_cast<void>(rule->get_Direction(&direction));
                    static_cast<void>(rule->get_Action(&action));
                    static_cast<void>(rule->get_Enabled(&enabled));
                    static_cast<void>(rule->get_Protocol(&protocol));
                    const QString rulePath = application == nullptr
                        ? QString()
                        : QDir::toNativeSeparators(QString::fromWCharArray(application));
                    const bool samePath =
                        rulePath.compare(wanted, Qt::CaseInsensitive) == 0;
                    if (samePath && direction == NET_FW_RULE_DIR_IN
                        && action == NET_FW_ACTION_ALLOW && enabled != VARIANT_FALSE
                        && (protocol == 17 || protocol == 256)) {
                        allowed = true;
                    } else if (!samePath) {
                        ++access.staleRules;
                    }
                }
                SysFreeString(name);
            }
            if (application != nullptr) {
                SysFreeString(application);
            }
        }
        VariantClear(&item);
        fetched = 0U;
    }

    if (allowed) {
        access.state = FirewallRuleState::Allowed;
    } else if ((activeProfiles & NET_FW_PROFILE2_PUBLIC) != 0) {
        // Adding a listening rule to a Public network is a deliberate choice,
        // not something to do quietly on the user's behalf.
        access.state = FirewallRuleState::PublicNetwork;
    } else if (access.staleRules > 0) {
        access.state = FirewallRuleState::StalePath;
    } else {
        access.state = FirewallRuleState::Missing;
    }
    return access;
}

bool requestFirewallRule() {
    const QString path = executablePath();
    if (path.isEmpty()) {
        return false;
    }

    // netsh is the Windows-supported way to do this from an elevated child, and
    // avoids shipping another signed-looking binary. The rule stays as narrow as
    // it can usefully be: this executable, inbound, UDP, allow, and only the
    // profiles where a home jam actually happens.
    //
    // Deleting first removes stale entries left by an update that moved the
    // executable, so repairs do not stack up duplicates.
    const QString command = QStringLiteral(
        "/c netsh advfirewall firewall delete rule name=\"JamLink\" >nul 2>&1 & "
        "netsh advfirewall firewall add rule name=\"JamLink\" "
        "dir=in action=allow program=\"%1\" protocol=UDP "
        "profile=private,domain enable=yes")
        .arg(path);

    const std::wstring parameters = command.toStdWString();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = L"cmd.exe";
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;

    if (ShellExecuteExW(&info) == FALSE || info.hProcess == nullptr) {
        // ERROR_CANCELLED is the user declining the UAC prompt, which is a
        // legitimate answer rather than a failure to report as a fault.
        const DWORD error = GetLastError();
        JAMLINK_LOG("firewall", error == ERROR_CANCELLED
            ? "user declined the elevation prompt"
            : "could not start the elevated firewall change");
        return false;
    }

    static_cast<void>(WaitForSingleObject(info.hProcess, 60'000U));
    DWORD exitCode = 1U;
    static_cast<void>(GetExitCodeProcess(info.hProcess, &exitCode));
    CloseHandle(info.hProcess);
    JAMLINK_LOG("firewall", exitCode == 0U
        ? "inbound UDP rule created for this build"
        : "the elevated firewall change reported failure");
    return exitCode == 0U;
}

} // namespace jamlink::desktop
