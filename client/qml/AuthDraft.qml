pragma Singleton
import QtQuick

// 认证页之间共享的临时草稿：只在内存里，不落盘。
// 登录页输入到一半去注册/找回，返回时邮箱、密码和自动登录选项都还在；
// 注册或改密成功后，这里留一条回执，登录页直接显示。
QtObject {
    property string user: ""
    property string email: ""
    property string password: ""
    property bool remember: true
    // 只有用户这次真的点过自动登录，才用草稿里的值覆盖已落盘的偏好
    property bool rememberTouched: false
    property string notice: ""

    function clearNotice() {
        if (notice.length > 0)
            notice = ""
    }

    function clearPassword() {
        password = ""
    }
}
