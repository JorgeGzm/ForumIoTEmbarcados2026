*** Settings ***
Documentation     Keywords to talk to the demo board the way a technician would:
...               the app shell on the USB port and the UART updater (MCUboot).
Library           SerialLibrary    encoding=ascii
Library           OperatingSystem
Library           Process
Library           String

*** Variables ***
${ROOT}           ${CURDIR}/../../..
${SHELL_PORT}     /dev/serial/by-id/usb-GZM_Embedded_Systems_Do_Codigo_ao_Campo_Demo-if00
${UPDATE_PORT}    /dev/serial/by-id/usb-1a86_USB_Single_Serial_5552003040-if00
${PROMPT}         uart:~$
${BOOT_TIMEOUT}   180s

*** Keywords ***
Open Board Shell
    [Documentation]    Wait for the app USB port (it comes back after every
    ...                reboot), let the boot log finish and open it. Safe to
    ...                call when it is already open.
    Close Board Shell
    Wait Until Created    ${SHELL_PORT}    ${BOOT_TIMEOUT}
    Wait Until Keyword Succeeds    10x    1s
    ...    Add Port    ${SHELL_PORT}    baudrate=115200    timeout=5
    # The boot log (with prompts in between) keeps coming for a moment
    Sleep    3s
    Reset Input Buffer
    Wait Until Keyword Succeeds    5x    1s    Run Shell Command    ${EMPTY}

Close Board Shell
    Run Keyword And Ignore Error    Delete All Ports

Run Shell Command
    [Documentation]    Send one shell command and return its output (what
    ...                comes after the echo of the command, up to the
    ...                prompt), without the ANSI color codes.
    [Arguments]    ${command}
    Reset Input Buffer
    Write Data    ${command}\r\n
    IF    $command
        Read Until    ${command}
    END
    ${raw}=    Read Until    ${PROMPT}
    ${text}=    Replace String Using Regexp    ${raw}    \\x1b\\[[0-9;]*m    ${EMPTY}
    Log    ${text}
    RETURN    ${text}

Reboot Board
    Write Data    kernel reboot cold\r\n
    Close Board Shell
    Wait Until Removed    ${SHELL_PORT}    15s
    Open Board Shell

Running Version Should Be
    [Arguments]    ${version}    ${state}
    ${out}=    Run Shell Command    boot version
    Should Contain    ${out}    app version: v${version} (${state})

User Count Should Be
    [Arguments]    ${expected}
    ${out}=    Run Shell Command    user count
    Should Contain    ${out}    ${expected} users

Pattern User
    [Documentation]    The naming pattern shared with the ztest suite
    ...                (tests/user_mgr/src/test_limit.c): id 1000+n,
    ...                "Usuario", "Teste<nn>", password "Senha<nn>".
    [Arguments]    ${n}
    ${nn}=    Evaluate    f"{int(${n}):02d}"
    ${id}=    Evaluate    1000 + int(${n})
    RETURN    ${id}    Teste${nn}    Senha${nn}

Add Pattern User
    [Arguments]    ${n}
    ${id}    ${last}    ${pwd}=    Pattern User    ${n}
    ${out}=    Run Shell Command    user add ${id} user Usuario ${last} ${pwd}
    RETURN    ${out}

Login Should Succeed
    [Arguments]    ${id}    ${pwd}
    ${out}=    Run Shell Command    user login ${id} ${pwd}
    Should Contain    ${out}    ok

Login Should Be Refused
    [Arguments]    ${id}    ${pwd}    ${code}=-13
    ${out}=    Run Shell Command    user login ${id} ${pwd}
    Should Contain    ${out}    login failed (${code})

Delete Pattern Users
    [Documentation]    Remove every pattern user (ids 1001-1050), ignoring
    ...                the ones that do not exist.
    FOR    ${n}    IN RANGE    1    51
        ${id}=    Evaluate    1000 + ${n}
        Run Shell Command    user del ${id}
    END

Update Firmware
    [Documentation]    Run our updater (tools/flasher/fw_update.py, the same
    ...                core as the GUI): reboot into MCUboot, upload to slot1,
    ...                image test, reset.
    [Arguments]    ${image}
    Close Board Shell
    ${r}=    Run Process    python3    ${ROOT}/tools/flasher/fw_update.py
    ...    -p    ${UPDATE_PORT}    -c    ${SHELL_PORT}    ${image}
    ...    timeout=300s    stderr=STDOUT
    Log    ${r.stdout}
    RETURN    ${r}

Reset Board From Recovery
    [Documentation]    Leave MCUboot serial recovery (it stays there after
    ...                a failed update) and boot the app again.
    Run Process    python3    ${ROOT}/tools/flasher/fw_update.py
    ...    -p    ${UPDATE_PORT}    --reset    timeout=30s
