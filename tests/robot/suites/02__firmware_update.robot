*** Settings ***
Documentation     Acceptance tests on the real board: the firmware update
...               through our UART updater (tools/flasher/fw_update.py, the same
...               core and SMP client as the GUI). Runs on its own: the setup
...               puts v1.0.0 back if needed and creates the users that test 08
...               expects to survive the update.
...               Needs the UART4 adapter (UPDATE_PORT), the USB console
...               (SHELL_PORT) and the images of the "West Build" (v1.0.0) and
...               "West Build (v2.0.0)" tasks.
Resource          ../resources/board.resource
Suite Setup       Prepare Board
Suite Teardown    Back To Factory State
Test Tags         acceptance    update

*** Variables ***
${IMG_V100}       ${ROOT}/app/build/app/zephyr/zephyr.signed.encrypted.bin
${IMG_V200}       ${ROOT}/app/build_v200/app/zephyr/zephyr.signed.encrypted.bin
${IMG_UNSIGNED}   ${ROOT}/app/build/app/zephyr/zephyr.bin
# Admin + these pattern users: what test 08 expects after the update
${USERS_BEFORE}   ${3}

*** Test Cases ***
06 An unsigned image is refused
    ${r}=    Update Firmware    ${IMG_UNSIGNED}
    Should Not Be Equal As Integers    ${r.rc}    0
    Reset Board From Recovery
    Open Board Shell
    Running Version Should Be    1.0.0    confirmed

07 An update that is not confirmed rolls back
    ${r}=    Update Firmware    ${IMG_V200}
    Should Be Equal As Integers    ${r.rc}    0
    Open Board Shell
    Running Version Should Be    2.0.0    test
    Reboot Board
    Running Version Should Be    1.0.0    confirmed

08 A confirmed update keeps the users
    [Documentation]    The users live on the SPI NOR, outside the MCUboot
    ...                slots: they must survive the swap to v2.0.0.
    ${r}=    Update Firmware    ${IMG_V200}
    Should Be Equal As Integers    ${r.rc}    0
    Open Board Shell
    Running Version Should Be    2.0.0    test
    Run Shell Command    boot confirm
    Reboot Board
    Running Version Should Be    2.0.0    confirmed
    ${total}=    Evaluate    ${USERS_BEFORE} + 1
    User Count Should Be    ${total}
    Login Should Succeed    100${USERS_BEFORE}    Senha0${USERS_BEFORE}

*** Keywords ***
Prepare Board
    Open Board Shell
    Factory Version Should Be Running
    Delete Pattern Users
    FOR    ${n}    IN RANGE    1    ${USERS_BEFORE + 1}
        ${out}=    Add Pattern User    ${n}
        Should Contain    ${out}    added
    END

Factory Version Should Be Running
    [Documentation]    Put v1.0.0 back (confirmed) if the board runs anything else.
    ${out}=    Run Shell Command    boot version
    IF    'v1.0.0 (confirmed)' not in $out
        ${r}=    Update Firmware    ${IMG_V100}
        Should Be Equal As Integers    ${r.rc}    0
        Open Board Shell
        Run Shell Command    boot confirm
    END

Back To Factory State
    [Documentation]    Leave the board as the demo expects it: only the admin,
    ...                and v1.0.0 confirmed.
    Open Board Shell
    Delete Pattern Users
    Factory Version Should Be Running
    Close Board Shell
