*** Settings ***
Documentation     How fast does a firmware image reach MCUboot over the UART?
...               The same signed image (the factory v1.0.0) goes to slot1 with
...               each SMP client: the mcumgr CLI and smpclient. There is no
...               "image test", so the board keeps running the factory version.
...               Needs the UART4 adapter (UPDATE_PORT) and the "West Build" image.
Resource          ../resources/board.resource
Suite Setup       Open Board Shell
Suite Teardown    Close Board Shell
Test Tags         speed

*** Variables ***
${IMAGE}              ${ROOT}/app/build/app/zephyr/zephyr.signed.encrypted.bin
${UPLOAD_HELPER}      ${ROOT}/tests/robot/libraries/upload_speed.py
# Limits with margin over what was measured with a 373 KB image at 921600 baud:
# mcumgr ~85 s (it sleeps 20 ms per 124-byte line), smpclient ~12 s.
${MAX_S_MCUMGR}       150
${MAX_S_SMPCLIENT}    30
${MIN_SPEEDUP}        3
${T_MCUMGR}           ${None}
${T_SMPCLIENT}        ${None}

*** Test Cases ***
01 Upload with the mcumgr CLI
    ${seconds}=    Upload Image With    mcumgr
    Set Suite Variable    ${T_MCUMGR}    ${seconds}
    Should Be True    ${seconds} < ${MAX_S_MCUMGR}

02 Upload with smpclient
    ${seconds}=    Upload Image With    smpclient
    Set Suite Variable    ${T_SMPCLIENT}    ${seconds}
    Should Be True    ${seconds} < ${MAX_S_SMPCLIENT}

03 smpclient is faster than mcumgr
    Skip If    $T_MCUMGR is None or $T_SMPCLIENT is None    an upload above failed
    ${speedup}=    Evaluate    round(${T_MCUMGR} / ${T_SMPCLIENT}, 1)
    Set Test Message    ${speedup}x faster (${T_MCUMGR} s → ${T_SMPCLIENT} s)
    Should Be True    ${speedup} >= ${MIN_SPEEDUP}

*** Keywords ***
Upload Image With
    [Documentation]    Upload ${IMAGE} to slot1 with one SMP client and return
    ...                the upload time in seconds. The board must come back on
    ...                the factory version afterwards.
    [Arguments]    ${client}
    File Should Exist    ${IMAGE}
    Close Board Shell
    ${r}=    Run Process    python3    ${UPLOAD_HELPER}
    ...    --client    ${client}    --image    ${IMAGE}
    ...    --uart    ${UPDATE_PORT}    --console    ${SHELL_PORT}
    ...    timeout=300s    stderr=STDOUT
    Log    ${r.stdout}
    Should Be Equal As Integers    ${r.rc}    0    upload with ${client} failed
    ${m}=    Get Regexp Matches    ${r.stdout}    UPLOAD_SECONDS=([0-9.]+) UPLOAD_KBPS=([0-9.]+)    1    2
    ${seconds}=    Convert To Number    ${m}[0][0]
    Set Test Message    ${client}: ${seconds} s · ${m}[0][1] KB/s
    Open Board Shell
    Running Version Should Be    1.0.0    confirmed
    RETURN    ${seconds}
