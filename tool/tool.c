#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>  // For drag-and-drop support

#include <stdio.h>

#define WINDOW_TITLE "NARF Filesystem Tool"

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void InitializeTreeView(HWND hwnd);
HTREEITEM AddItemToTree(HWND hTree, HTREEITEM hParent, const char *text);
void AddSampleItems();
void ResizeTreeView(HWND hwnd);
void HandleFileDrop(HWND hwnd, HDROP hDrop);

HTREEITEM hDraggedItem = NULL; // Store the item being dragged
HWND hTreeView;
HIMAGELIST hDragImageList;
POINT ptOffset;
HIMAGELIST hImageList;

// Create an image list for the TreeView and associate an HBITMAP
void CreateImageList(HWND hwnd) {
    hImageList = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 1, 1);  // Create image list (16x16 icons)

    // Create a simple 16x16 bitmap dynamically (you can replace this with your own logic)
    HBITMAP hBitmap = CreateCompatibleBitmap(GetDC(hwnd), 16, 16);

    if (hBitmap == NULL) {
        MessageBox(NULL, "Failed to create bitmap!", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    // Create a memory DC and select the bitmap into it
    HDC hdcMem = CreateCompatibleDC(GetDC(hwnd));
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

    // Draw something simple into the bitmap (for example, a red rectangle)
    RECT rect = { 0, 0, 16, 16 };
    HBRUSH hBrush = CreateSolidBrush(RGB(255, 0, 0));  // Red color
    FillRect(hdcMem, &rect, hBrush);

    // Clean up
    SelectObject(hdcMem, hOldBitmap);
    DeleteDC(hdcMem);
    DeleteObject(hBrush);

    // Add the bitmap to the image list
    ImageList_Add(hImageList, hBitmap, NULL);

    // Set the image list for the TreeView control
    TreeView_SetImageList(hTreeView, hImageList, TVSIL_NORMAL);  // Set the normal image list
}

void InitializeTreeView(HWND hwnd) {
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_TREEVIEW_CLASSES;
    InitCommonControlsEx(&icex);
    
    hTreeView = CreateWindowEx(WS_EX_CLIENTEDGE, WC_TREEVIEW, "", 
                               WS_CHILD | WS_VISIBLE | TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT, 
                               10, 10, 760, 500, hwnd, (HMENU) 1, GetModuleHandle(NULL), NULL);
    
    if (!hTreeView) {
        MessageBox(hwnd, "TreeView creation failed!", "Error", MB_OK | MB_ICONERROR);
        return;
    }

    CreateImageList(hwnd);
    
    ShowWindow(hTreeView, SW_SHOW);
    UpdateWindow(hTreeView);

    // Enable drag-and-drop
    DragAcceptFiles(hwnd, TRUE);
}

HTREEITEM AddItemToTree(HWND hTree, HTREEITEM hParent, const char *text) {
    TVINSERTSTRUCT tvi = {0};
    tvi.hParent = hParent;
    tvi.hInsertAfter = TVI_LAST;
    tvi.item.mask = TVIF_TEXT;
    tvi.item.pszText = (LPSTR)text;
    return TreeView_InsertItem(hTree, &tvi);
}

void AddSampleItems() {
    HTREEITEM hRoot = AddItemToTree(hTreeView, NULL, "Root Directory");
    HTREEITEM hDir = AddItemToTree(hTreeView, hRoot, "Example Directory");
    AddItemToTree(hTreeView, hDir, "Subfile.txt");
    AddItemToTree(hTreeView, hRoot, "Example File.txt");
}

void ResizeTreeView(HWND hwnd) {
    if (hTreeView) {
        RECT rcClient;
        GetClientRect(hwnd, &rcClient);
        MoveWindow(hTreeView, 10, 10, rcClient.right - 20, rcClient.bottom - 20, TRUE);
    }
}

void HandleFileDrop(HWND hwnd, HDROP hDrop) {
    UINT fileCount = DragQueryFile(hDrop, 0xFFFFFFFF, NULL, 0); // Get the number of files dropped
    if (fileCount == 0) return;

    // Get the file path of the dropped file
    char filePath[MAX_PATH];
    DragQueryFile(hDrop, 0, filePath, sizeof(filePath));

    // Find the currently selected item in the tree view
    HTREEITEM hSelectedItem = TreeView_GetSelection(hTreeView);
 
    // If no item is selected, default to the root directory
    if (!hSelectedItem) {
        hSelectedItem = TreeView_GetRoot(hTreeView);
    }
    if (hSelectedItem) {
        AddItemToTree(hTreeView, hSelectedItem, filePath);

        // Ensure the parent item is expanded to show the new file
        TreeView_Expand(hTreeView, hSelectedItem, TVE_EXPAND);
    }

    // Clean up drag-and-drop data
    DragFinish(hDrop);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "NARFFSToolClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    
    RegisterClass(&wc);
    
    HWND hwnd = CreateWindow(wc.lpszClassName, WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, NULL, NULL, hInstance, NULL);
    
    if (!hwnd) return 0;
    
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    
    InitializeTreeView(hwnd);
    AddSampleItems();
    
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_DROPFILES) {
            HandleFileDrop(hwnd, (HDROP)msg.wParam);  // Use msg.wParam for file drop
        }
        if (msg.message == WM_KEYDOWN && msg.hwnd == hTreeView) {
            // If key press is in the tree view window, process it
            WindowProc(hwnd, msg.message, msg.wParam, msg.lParam);
        } else {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    
    return (int) msg.wParam;
}

// Function to get the text of a TreeView item
const char* TreeView_GetItemText(HWND hTree, HTREEITEM hItem) {
    static char itemText[256]; // Buffer to hold the item text
    TVITEM tvi = {0};
    tvi.hItem = hItem;
    tvi.mask = TVIF_TEXT;
    tvi.pszText = itemText;
    tvi.cchTextMax = sizeof(itemText) - 1;

    if (TreeView_GetItem(hTree, &tvi)) {
        return itemText;
    }
    return NULL;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    static HTREEITEM hLastHoveredItem = NULL;
    switch (uMsg) {
        case WM_SIZE:
            ResizeTreeView(hwnd);
            return 0;
        case WM_DROPFILES:
            HandleFileDrop(hwnd, (HDROP)wParam);  // Handle file drop in the WindowProc
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_DELETE) {  // Check if the Delete key is pressed
                HTREEITEM hSelectedItem = TreeView_GetSelection(hTreeView);
                if (hSelectedItem) {
                    TreeView_DeleteItem(hTreeView, hSelectedItem);  // Remove selected item
                }
                return 0;
            }
            break;
        case WM_LBUTTONUP: // Handle the drop when the left mouse button is released
            if (hDraggedItem) {
                // Find the drop target item (the item under the cursor)
                TVHITTESTINFO hitTestInfo = {0};
                hitTestInfo.pt.x = GET_X_LPARAM(lParam);
                hitTestInfo.pt.y = GET_Y_LPARAM(lParam);
                TreeView_HitTest(hTreeView, &hitTestInfo);

                if (hitTestInfo.hItem && hitTestInfo.hItem != hDraggedItem) {
                    // Move the dragged item to the new location
                    //HTREEITEM hParent = TreeView_GetParent(hTreeView, hitTestInfo.hItem);
                    //AddItemToTree(hTreeView, hParent, (const char*)TreeView_GetItemText(hTreeView, hDraggedItem));
                    AddItemToTree(hTreeView, hitTestInfo.hItem, (const char*)TreeView_GetItemText(hTreeView, hDraggedItem));
                    TreeView_DeleteItem(hTreeView, hDraggedItem);
        // Ensure the parent item is expanded to show the new file
        TreeView_Expand(hTreeView, hitTestInfo.hItem, TVE_EXPAND);
                }

                if (hLastHoveredItem) {
                    TVITEM tvi = {0};
                    tvi.mask = TVIF_STATE;
                    tvi.hItem = hLastHoveredItem;
                    tvi.state = 0;
                    tvi.stateMask = TVIS_DROPHILITED; // Reset the selection state
                    TreeView_SetItem(hTreeView, &tvi);
               	    hLastHoveredItem = NULL;
                }

                // Redraw the treeview to reflect the updated appearance
                InvalidateRect(hTreeView, NULL, TRUE);

                hDraggedItem = NULL; // Reset the dragged item
                ReleaseCapture();

                // Clean up the drag image resources
                ImageList_Destroy(hDragImageList);
                hDragImageList = NULL;
            }
            break;
        case WM_MOUSEMOVE: // Update the dragged item during the drag operation
            if (hDraggedItem) {
                // Manual extraction of mouse coordinates
                int x = LOWORD(lParam);
                int y = HIWORD(lParam);

                // Move the drag image with the mouse
                int dx = x - ptOffset.x;
                int dy = y - ptOffset.y;

                ptOffset.x = x;
                ptOffset.y = y;

                // Move the drag image
                ImageList_DragMove(dx, dy);

                // Find the item under the mouse cursor
                TVHITTESTINFO hitTestInfo = {0};
                hitTestInfo.pt.x = x;
                hitTestInfo.pt.y = y;
                HTREEITEM hHoveredItem = TreeView_HitTest(hTreeView, &hitTestInfo);

                // If a new item is under the cursor, highlight it
                if (hHoveredItem != hLastHoveredItem) {

                    TreeView_Expand(hTreeView, hHoveredItem, TVE_EXPAND);

                    // Reset the previous item's state if any
                    if (hLastHoveredItem) {
                        TVITEM tvi = {0};
                        tvi.mask = TVIF_STATE;
                        tvi.hItem = hLastHoveredItem;
                        tvi.state = 0;
                        tvi.stateMask = TVIS_DROPHILITED; // Reset the selection state
                        TreeView_SetItem(hTreeView, &tvi);
                    }

                    // Set the new item's state
                    TVITEM tvi = {0};
                    tvi.mask = TVIF_STATE;
                    tvi.hItem = hHoveredItem;
                    tvi.state = TVIS_DROPHILITED;
                    tvi.stateMask = TVIS_DROPHILITED; // Reset the selection state
                    TreeView_SetItem(hTreeView, &tvi);
                    
                    // Update the last hovered item
                    hLastHoveredItem = hHoveredItem;

                    // Redraw the treeview to reflect the updated appearance
                    InvalidateRect(hTreeView, NULL, TRUE);
                }

            }
            break;
        case WM_RBUTTONUP:
        case WM_LBUTTONDOWN:
            break;
        case WM_NOTIFY:
            if (((LPNMHDR)lParam)->code == TVN_BEGINDRAG) {
                // This message is sent when dragging starts
                NMTREEVIEW *pNMTV = (NMTREEVIEW*)lParam;
                hDraggedItem = pNMTV->itemNew.hItem;

		// Select the item to get the drag image
                TreeView_SelectItem(hTreeView, hDraggedItem);

                // Create the drag image
                hDragImageList = (HIMAGELIST)SendMessage(hTreeView, TVM_CREATEDRAGIMAGE, 0, (LPARAM)hDraggedItem);

                if (hDragImageList) {
                    // Set the drag image position (mouse position at the time of drag start)
                    int x = LOWORD(lParam);
                    int y = HIWORD(lParam);
                    ptOffset.x = x;
                    ptOffset.y = y;

                    POINT pt;
                    GetCursorPos(&pt);
                    ScreenToClient(hwnd, &pt);
                    //ImageList_BeginDrag(hDragImageList, 0, pt.x, pt.y);
                    ImageList_BeginDrag(hDragImageList, 0, x, y);

                    // Begin dragging the image
                    //ImageList_BeginDrag(hDragImageList, 0, 0, 0); // Start drag with image list and offset
                }
else { printf("NO IMAGE %08x\n", hDraggedItem); }

                SetCapture(hwnd); // Capture mouse events while dragging
                hLastHoveredItem = NULL;
            }
            break;
        case WM_DESTROY:
            if (hDragImageList) {
                ImageList_Destroy(hDragImageList);
            }
            PostQuitMessage(0);
            return 0;


    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
