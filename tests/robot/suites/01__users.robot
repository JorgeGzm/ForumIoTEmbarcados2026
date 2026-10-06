*** Settings ***
Documentation     Acceptance tests on the real board: the user store (gzm
...               user_mgr on LittleFS) through the USB shell, the way a
...               technician would use it. Same naming pattern as the ztest
...               suite tests/user_mgr. Needs only the USB console (SHELL_PORT).
Resource          ../resources/board.resource
Suite Setup       Prepare Board
Suite Teardown    Clean Up Users
Test Tags         acceptance    users    smoke

*** Variables ***
${USERS_MAX}      50

*** Test Cases ***
01 Boots the factory version
    Running Version Should Be    1.0.0    confirmed
    Login Should Succeed    1    1234

02 Creates a user who can log in
    ${out}=    Add Pattern User    1
    Should Contain    ${out}    user 1001 added
    Login Should Succeed    1001    Senha01
    Login Should Be Refused    1001    errada

03 A blocked user cannot log in
    ${out}=    Run Shell Command    user set_status 1001 0
    Should Contain    ${out}    user 1001 blocked
    Login Should Be Refused    1001    Senha01
    Run Shell Command    user set_status 1001 1
    Login Should Succeed    1001    Senha01

04 Deletes a user
    ${out}=    Run Shell Command    user del 1001
    Should Contain    ${out}    user 1001 deleted
    Login Should Be Refused    1001    Senha01    -2

05 The store refuses user 51
    [Documentation]    Admin + 49 pattern users = 50; the next one gets -ENOSPC.
    FOR    ${n}    IN RANGE    1    ${USERS_MAX}
        ${out}=    Add Pattern User    ${n}
        Should Contain    ${out}    added
    END
    User Count Should Be    ${USERS_MAX}
    ${out}=    Add Pattern User    50
    Should Contain    ${out}    add failed (-28)
    User Count Should Be    ${USERS_MAX}

*** Keywords ***
Prepare Board
    Open Board Shell
    Delete Pattern Users

Clean Up Users
    [Documentation]    Leave only the admin, as the demo expects.
    Open Board Shell
    Delete Pattern Users
    Close Board Shell
