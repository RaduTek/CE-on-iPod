//
// panelkeys.h - the keyboard for the buttons of the bring-up panels
// (dskinfo, wavpanel), so they can be worked with the click wheel
// (whlkeys sends real key presses with keybd_event).
//
// The panels are plain windows, not dialogs, and IsDialogMessage did not
// give their buttons Tab or Enter on CE 7 (tried 2026-11-26). So the panel's
// message loop hands every message to PanelKeys_Handle first:
//
//   Tab, Down, Right       the focus to the next button (wraps)
//   Shift+Tab, Up, Left    the focus to the previous button
//   Enter, Space           presses the button with the focus, once, when
//                          the key is let go (WM_COMMAND BN_CLICKED to the
//                          panel, as a click would)
//
// Other keys (Escape, ...) are left alone.
//
#ifndef PANELKEYS_H
#define PANELKEYS_H

// TRUE when pMsg was one of the keys above for this panel and has been
// dealt with: the caller does not dispatch it then.
static BOOL PanelKeys_Handle(HWND hPanel, const HWND *pButtons, int count, const MSG *pMsg)
{
    HWND hFocus;
    int i, cur = -1, next;
    BOOL back = FALSE;

    if (pMsg->message != WM_KEYDOWN && pMsg->message != WM_KEYUP)
        return FALSE;
    if (pMsg->hwnd != hPanel && !IsChild(hPanel, pMsg->hwnd))
        return FALSE;

    hFocus = GetFocus();
    for (i = 0; i < count; i++)
    {
        if (pButtons[i] != NULL && pButtons[i] == hFocus)
            cur = i;
    }

    switch (pMsg->wParam)
    {
    case VK_TAB:
        back = (GetKeyState(VK_SHIFT) < 0);
        break;
    case VK_DOWN:
    case VK_RIGHT:
        break;
    case VK_UP:
    case VK_LEFT:
        back = TRUE;
        break;

    case VK_RETURN:
    case VK_SPACE:
        // the button's own handling of Space would press it a second time
        if (pMsg->message == WM_KEYUP && cur >= 0)
            SendMessage(hPanel, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(pButtons[cur]), BN_CLICKED),
                        (LPARAM)pButtons[cur]);
        return TRUE;

    default:
        return FALSE;
    }

    // a focus key: move on key down only
    if (pMsg->message == WM_KEYDOWN && count > 0)
    {
        if (cur < 0)
            next = back ? count - 1 : 0;
        else
            next = back ? (cur + count - 1) % count : (cur + 1) % count;
        if (pButtons[next] != NULL)
            SetFocus(pButtons[next]);
    }
    return TRUE;
}

#endif
