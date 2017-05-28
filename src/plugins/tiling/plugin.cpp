#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#include <map>
#include <vector>

#include "../../api/plugin_api.h"
#include "../../common/accessibility/display.h"
#include "../../common/accessibility/application.h"
#include "../../common/accessibility/window.h"
#include "../../common/accessibility/element.h"
#include "../../common/accessibility/observer.h"
#include "../../common/ipc/daemon.h"
#include "../../common/misc/carbon.h"
#include "../../common/misc/assert.h"
#include "../../common/misc/debug.h"
#include "../../common/config/cvar.h"

#include "config.h"
#include "region.h"
#include "node.h"
#include "vspace.h"
#include "controller.h"
#include "constants.h"
#include "misc.h"

#define internal static
#define local_persist static

typedef std::map<pid_t, macos_application *> macos_application_map;
typedef macos_application_map::iterator macos_application_map_it;

typedef std::map<uint32_t, macos_window *> macos_window_map;
typedef macos_window_map::iterator macos_window_map_it;

#define CGSDefaultConnection _CGSDefaultConnection()
typedef int CGSConnectionID;
extern "C" CGSConnectionID _CGSDefaultConnection(void);
extern "C" CGError CGSGetOnScreenWindowCount(const CGSConnectionID CID, CGSConnectionID TID, int *Count);
extern "C" CGError CGSGetOnScreenWindowList(const CGSConnectionID CID, CGSConnectionID TID, int Count, int *List, int *OutCount);

// TODO(koekeishiya): Shorter name.
#define CONFIG_FILE "/.chunkwmtilingrc"

internal const char *PluginName = "Tiling";
internal const char *PluginVersion = "0.0.2";

internal macos_application_map Applications;

/* TODO(koekeishiya): All functions operating on this structure must be made thread-safe:
 * macos_window *GetWindowByID(uint32_t Id)
 * AddWindowToCollection(macos_window *Window)
 * RemoveWindowFromCollection(macos_window *Window)
 * */
internal macos_window_map Windows;

plugin_broadcast *ChunkWMBroadcastEvent;

/* NOTE(koekeishiya): We need a way to retrieve AXUIElementRef from a CGWindowID.
 * There is no way to do this, without caching AXUIElementRef references.
 * Here we perform a lookup of macos_window structs. */
macos_window *GetWindowByID(uint32_t Id)
{
    macos_window_map_it It = Windows.find(Id);
    return It != Windows.end() ? It->second : NULL;
}

// NOTE(koekeishiya): Caller is responsible for making sure that the window is not a dupe.
internal void
AddWindowToCollection(macos_window *Window)
{
    Windows[Window->Id] = Window;
}

internal macos_window *
RemoveWindowFromCollection(macos_window *Window)
{
    macos_window *Result = GetWindowByID(Window->Id);
    if(Result)
    {
        Windows.erase(Window->Id);
    }
    return Result;
}

internal void
ClearWindowCache()
{
    for(macos_window_map_it It = Windows.begin();
        It != Windows.end();
        ++It)
    {
        macos_window *Window = It->second;
        AXLibDestroyWindow(Window);
    }
    Windows.clear();
}

internal void
AddApplicationWindowList(macos_application *Application)
{
    macos_window **WindowList = AXLibWindowListForApplication(Application);
    if(!WindowList)
    {
        return;
    }

    macos_window **List = WindowList;
    macos_window *Window;
    while((Window = *List++))
    {
        if(GetWindowByID(Window->Id))
        {
            AXLibDestroyWindow(Window);
        }
        else
        {
            AddWindowToCollection(Window);
        }
    }

    free(WindowList);
}

internal void
UpdateWindowCollection()
{
    for(macos_application_map_it It = Applications.begin();
        It != Applications.end();
        ++It)
    {
        macos_application *Application = It->second;
        AddApplicationWindowList(Application);
    }
}

internal void
AddApplication(macos_application *Application)
{
    Applications[Application->PID] = Application;
}

internal void
RemoveApplication(macos_application *Application)
{
    macos_application_map_it It = Applications.find(Application->PID);
    if(It != Applications.end())
    {
        macos_application *Copy = It->second;
        AXLibDestroyApplication(Copy);

        Applications.erase(Application->PID);
    }
}

internal void
ClearApplicationCache()
{
    for(macos_application_map_it It = Applications.begin();
        It != Applications.end();
        ++It)
    {
        macos_application *Application = It->second;
        AXLibDestroyApplication(Application);
    }
    Applications.clear();
}

bool IsWindowValid(macos_window *Window)
{
    bool Result = ((AXLibIsWindowStandard(Window)) &&
                   (AXLibHasFlags(Window, Window_Movable)) &&
                   (AXLibHasFlags(Window, Window_Resizable)) &&
                   (!AXLibHasFlags(Window, Window_Invalid)));
    return Result;
}

internal bool
TileWindowPreValidation(macos_window *Window)
{
    if(AXLibHasFlags(Window, Window_Float))
    {
        return false;
    }

    if(!IsWindowValid(Window))
    {
        FloatWindow(Window);
        return false;
    }

    if(CVarIntegerValue(CVAR_WINDOW_FLOAT_NEXT))
    {
        FloatWindow(Window);
        UpdateCVar(CVAR_WINDOW_FLOAT_NEXT, 0);
        return false;
    }

    return true;
}

// NOTE(koekeishiya): Caller is responsible for making sure that the window is a valid window
// that we can properly manage. The given macos_space must also be of type kCGSSpaceUser,
// meaning that it is a space we can legally interact with.
void TileWindowOnSpace(macos_window *Window, macos_space *Space, virtual_space *VirtualSpace)
{
    CFStringRef DisplayRef;

    if(VirtualSpace->Mode == Virtual_Space_Float)
    {
        goto out;
    }

    /* NOTE(koekeishiya): This function appears to always return a valid identifier!
     * Could this potentially return NULL if an invalid CGSSpaceID is passed ?
     * The function returns NULL if "Displays have separate spaces" is disabled !!! */
    DisplayRef = AXLibGetDisplayIdentifierFromSpace(Space->Id);
    ASSERT(DisplayRef);

    if(AXLibIsDisplayChangingSpaces(DisplayRef))
    {
        goto display_free;
    }

    if(VirtualSpace->Tree)
    {
        node *Exists = GetNodeWithId(VirtualSpace->Tree, Window->Id, VirtualSpace->Mode);
        if(Exists)
        {
            goto display_free;
        }

        node *Node = NULL;
        uint32_t InsertionPoint = CVarIntegerValue(CVAR_BSP_INSERTION_POINT);

        if(VirtualSpace->Mode == Virtual_Space_Bsp)
        {
            Node = GetFirstMinDepthPseudoLeafNode(VirtualSpace->Tree);
            if(Node)
            {
                if(Node->Parent)
                {
                    node_ids NodeIds = AssignNodeIds(Node->Parent->WindowId, Window->Id);
                    Node->Parent->WindowId = Node_Root;
                    Node->Parent->Left->WindowId = NodeIds.Left;
                    Node->Parent->Right->WindowId = NodeIds.Right;
                    CreateNodeRegionRecursive(Node->Parent, false, Space, VirtualSpace);
                    ApplyNodeRegion(Node->Parent, VirtualSpace->Mode);
                }
                else
                {
                    Node->WindowId = Window->Id;
                    CreateNodeRegion(Node, Region_Full, Space, VirtualSpace);
                    ApplyNodeRegion(Node, VirtualSpace->Mode);
                }
                goto display_free;
            }

            if(InsertionPoint)
            {
                Node = GetNodeWithId(VirtualSpace->Tree, InsertionPoint, VirtualSpace->Mode);
            }

            if(!Node)
            {
                Node = GetFirstMinDepthLeafNode(VirtualSpace->Tree);
                ASSERT(Node != NULL);
            }

            node_split Split = (node_split) CVarIntegerValue(CVAR_BSP_SPLIT_MODE);
            if(Split == Split_Optimal)
            {
                Split = OptimalSplitMode(Node);
            }

            CreateLeafNodePair(Node, Node->WindowId, Window->Id, Split, Space, VirtualSpace);
            ApplyNodeRegion(Node, VirtualSpace->Mode);

            // NOTE(koekeishiya): Reset fullscreen-zoom state.
            if(VirtualSpace->Tree->Zoom)
            {
                VirtualSpace->Tree->Zoom = NULL;
            }
        }
        else if(VirtualSpace->Mode == Virtual_Space_Monocle)
        {
            if(InsertionPoint)
            {
                Node = GetNodeWithId(VirtualSpace->Tree, InsertionPoint, VirtualSpace->Mode);
            }

            if(!Node)
            {
                Node = GetLastLeafNode(VirtualSpace->Tree);
                ASSERT(Node != NULL);
            }

            node *NewNode = CreateRootNode(Window->Id, Space, VirtualSpace);

            if(Node->Right)
            {
                node *Next = Node->Right;
                Next->Left = NewNode;
                NewNode->Right = Next;
            }

            NewNode->Left = Node;
            Node->Right = NewNode;
            ResizeWindowToRegionSize(NewNode);
        }
    }
    else
    {
        char *Buffer;
        if((ShouldDeserializeVirtualSpace(VirtualSpace)) &&
           ((Buffer = ReadFile(VirtualSpace->TreeLayout))))
        {
            VirtualSpace->Tree = DeserializeNodeFromBuffer(Buffer);
            VirtualSpace->Tree->WindowId = Window->Id;
            CreateNodeRegion(VirtualSpace->Tree, Region_Full, Space, VirtualSpace);
            CreateNodeRegionRecursive(VirtualSpace->Tree, false, Space, VirtualSpace);
            ResizeWindowToRegionSize(VirtualSpace->Tree);
            free(Buffer);
        }
        else
        {
            // NOTE(koekeishiya): This path is equal for both bsp and monocle spaces!
            VirtualSpace->Tree = CreateRootNode(Window->Id, Space, VirtualSpace);
            ResizeWindowToRegionSize(VirtualSpace->Tree);
        }
    }

display_free:
    CFRelease(DisplayRef);
out:;
}

void TileWindow(macos_window *Window)
{
    if(TileWindowPreValidation(Window))
    {
        macos_space *Space;
        bool Success = AXLibActiveSpace(&Space);
        ASSERT(Success);

        if(Space->Type == kCGSSpaceUser)
        {
            virtual_space *VirtualSpace = AcquireVirtualSpace(Space);
            TileWindowOnSpace(Window, Space, VirtualSpace);
            ReleaseVirtualSpace(VirtualSpace);
        }

        AXLibDestroySpace(Space);
    }
}

internal bool
UntileWindowPreValidation(macos_window *Window)
{
    if(AXLibHasFlags(Window, Window_Float))
    {
        return false;
    }

    if(!IsWindowValid(Window))
    {
        return false;
    }

    return true;
}

// NOTE(koekeishiya): Caller is responsible for making sure that the window is a valid window
// that we can properly manage. The given macos_space must also be of type kCGSSpaceUser,
// meaning that it is a space we can legally interact with.
void UntileWindowFromSpace(macos_window *Window, macos_space *Space, virtual_space *VirtualSpace)
{
    if((!VirtualSpace->Tree) ||
       (VirtualSpace->Mode == Virtual_Space_Float))
    {
        return;
    }

    node *Node = GetNodeWithId(VirtualSpace->Tree, Window->Id, VirtualSpace->Mode);
    if(!Node)
    {
        return;
    }

    if(VirtualSpace->Mode == Virtual_Space_Bsp)
    {
        /* NOTE(koekeishiya): The window was in fullscreen-zoom.
         * We need to null the pointer to prevent a potential bug. */
        if(VirtualSpace->Tree->Zoom == Node)
        {
            VirtualSpace->Tree->Zoom = NULL;
        }

        if(Node->Parent && Node->Parent->Left && Node->Parent->Right)
        {
            /* NOTE(koekeishiya): The window was in parent-zoom.
             * We need to null the pointer to prevent a potential bug. */
            if(Node->Parent->Zoom == Node)
            {
                Node->Parent->Zoom = NULL;
            }

            node *NewLeaf = Node->Parent;
            node *RemainingLeaf = IsRightChild(Node) ? Node->Parent->Left
                                                     : Node->Parent->Right;
            NewLeaf->Left = NULL;
            NewLeaf->Right = NULL;
            NewLeaf->Zoom = NULL;

            NewLeaf->WindowId = RemainingLeaf->WindowId;
            if(RemainingLeaf->Left && RemainingLeaf->Right)
            {
                NewLeaf->Left = RemainingLeaf->Left;
                NewLeaf->Left->Parent = NewLeaf;

                NewLeaf->Right = RemainingLeaf->Right;
                NewLeaf->Right->Parent = NewLeaf;

                CreateNodeRegionRecursive(NewLeaf, true, Space, VirtualSpace);
            }

            /* NOTE(koekeishiya): Re-zoom window after spawned window closes.
             * see reference: https://github.com/koekeishiya/chunkwm/issues/20 */
            ApplyNodeRegion(NewLeaf, VirtualSpace->Mode);
            if(NewLeaf->Parent && NewLeaf->Parent->Zoom)
            {
                ResizeWindowToExternalRegionSize(NewLeaf->Parent->Zoom,
                                                 NewLeaf->Parent->Region);
            }

            free(RemainingLeaf);
            free(Node);
        }
        else if(!Node->Parent)
        {
            free(VirtualSpace->Tree);
            VirtualSpace->Tree = NULL;
        }
    }
    else if(VirtualSpace->Mode == Virtual_Space_Monocle)
    {
        node *Prev = Node->Left;
        node *Next = Node->Right;

        if(Prev)
        {
            Prev->Right = Next;
        }

        if(Next)
        {
            Next->Left = Prev;
        }

        if(Node == VirtualSpace->Tree)
        {
            VirtualSpace->Tree = Next;
        }

        free(Node);
    }
}

void UntileWindow(macos_window *Window)
{
    if(UntileWindowPreValidation(Window))
    {
        CFStringRef DisplayRef = AXLibGetDisplayIdentifierFromWindowRect(Window->Position, Window->Size);
        ASSERT(DisplayRef);

        // TODO(koekeishiya): We do not want to request the active space here,
        // but we need to get the space that has this window. I doubt this
        // information is available through the API after the window has been
        // marked as destroyed by the WindowServer. We could cache this information,
        // but that is not an ideal solution, because a window may frequently be moved
        // between spaces, and could even belong to multiple spaces.
        //
        // We probably want to delegate this responsibility to
        //  RebalanceWindowTree()
        // as this function will trigger upon next space entrance.

        macos_space *Space = AXLibActiveSpace(DisplayRef);
        ASSERT(Space);

        if(Space->Type == kCGSSpaceUser)
        {
            virtual_space *VirtualSpace = AcquireVirtualSpace(Space);
            UntileWindowFromSpace(Window, Space, VirtualSpace);
            ReleaseVirtualSpace(VirtualSpace);
        }

        AXLibDestroySpace(Space);
        CFRelease(DisplayRef);
    }
}

/* NOTE(koekeishiya): Returns a vector of CGWindowIDs. */
std::vector<uint32_t> GetAllVisibleWindowsForSpace(macos_space *Space, bool IncludeInvalidWindows, bool IncludeFloatingWindows)
{
    bool Success;
    CGError Error;
    int WindowCount, *WindowList;
    std::vector<uint32_t> Result;

    Error = CGSGetOnScreenWindowCount(CGSDefaultConnection, 0, &WindowCount);
    if(Error != kCGErrorSuccess)
    {
        fprintf(stderr, "CGSGetOnScreenWindowCount failed..\n");
        goto out;
    }

    WindowList = (int *) malloc(sizeof(int) * WindowCount);
    ASSERT(WindowList);

    Error = CGSGetOnScreenWindowList(CGSDefaultConnection, 0, WindowCount, WindowList, &WindowCount);
    if(Error != kCGErrorSuccess)
    {
        fprintf(stderr, "CGSGetOnScreenWindowList failed..\n");
        goto windowlist_free;
    }

    unsigned DesktopId;
    Success = AXLibCGSSpaceIDToDesktopID(Space->Id, NULL, &DesktopId);
    ASSERT(Success);

    for(int Index = 0; Index < WindowCount; ++Index)
    {
        uint32_t WindowId = WindowList[Index];

        if(!AXLibSpaceHasWindow(Space->Id, WindowId))
        {
            /* NOTE(koekeishiya): The onscreenwindowlist can contain windowids
             * that we do not care about. Check that the window in question is
             * in our cache and on the correct monitor. */
            continue;
        }

        macos_window *Window = GetWindowByID(WindowId);
        if(!Window)
        {
            // NOTE(koekeishiya): The chunkwm core does not report these windows to
            // plugins, and they are therefore never cached, we simply ignore them.
            // DEBUG_PRINT("   %d:window not cached\n", WindowId);
            continue;
        }

        if(IsWindowValid(Window) || IncludeInvalidWindows)
        {
            printf("%d:desktop   %d:%d:%s:%s\n",
                    DesktopId,
                    Window->Id,
                    Window->Level,
                    Window->Owner->Name,
                    Window->Name);
            if((!AXLibHasFlags(Window, Window_Float)) ||
               (IncludeFloatingWindows))
            {
                Result.push_back(Window->Id);
            }
        }
        else
        {
            printf("%d:desktop   %d:%d:invalid window:%s:%s\n",
                    DesktopId,
                    Window->Id,
                    Window->Level,
                    Window->Owner->Name,
                    Window->Name);
        }
    }

windowlist_free:
    free(WindowList);

out:
    return Result;
}

std::vector<uint32_t> GetAllVisibleWindowsForSpace(macos_space *Space)
{
    return GetAllVisibleWindowsForSpace(Space, false, false);
}

internal std::vector<uint32_t>
GetAllWindowsInTree(node *Tree, virtual_space_mode VirtualSpaceMode)
{
    std::vector<uint32_t> Windows;

    node *Node = GetFirstLeafNode(Tree);
    while(Node)
    {
        if(IsLeafNode(Node))
        {
            Windows.push_back(Node->WindowId);
        }

        if(VirtualSpaceMode == Virtual_Space_Bsp)
        {
            Node = GetNearestNodeToTheRight(Node);
        }
        else if(VirtualSpaceMode == Virtual_Space_Monocle)
        {
            Node = Node->Right;
        }
    }

    return Windows;
}

internal std::vector<uint32_t>
GetAllWindowsToAddToTree(std::vector<uint32_t> &VisibleWindows, std::vector<uint32_t> &WindowsInTree)
{
    std::vector<uint32_t> Windows;
    for(size_t WindowIndex = 0;
        WindowIndex < VisibleWindows.size();
        ++WindowIndex)
    {
        bool Found = false;
        uint32_t WindowId = VisibleWindows[WindowIndex];

        for(size_t Index = 0;
            Index < WindowsInTree.size();
            ++Index)
        {
            if(WindowId == WindowsInTree[Index])
            {
                Found = true;
                break;
            }
        }

        if((!Found) && (!AXLibStickyWindow(WindowId)))
        {
            Windows.push_back(WindowId);
        }
    }

    return Windows;
}

internal std::vector<uint32_t>
GetAllWindowsToRemoveFromTree(std::vector<uint32_t> &VisibleWindows, std::vector<uint32_t> &WindowsInTree)
{
    std::vector<uint32_t> Windows;
    for(size_t Index = 0;
        Index < WindowsInTree.size();
        ++Index)
    {
        bool Found = false;
        uint32_t WindowId = WindowsInTree[Index];

        for(size_t WindowIndex = 0;
            WindowIndex < VisibleWindows.size();
            ++WindowIndex)
        {
            if(VisibleWindows[WindowIndex] == WindowId)
            {
                Found = true;
                break;
            }
        }

        if(!Found)
        {
            Windows.push_back(WindowsInTree[Index]);
        }
    }

    return Windows;
}

/* NOTE(koekeishiya): The caller is responsible for making sure that the space
 * passed to this function is of type kCGSSpaceUser, and that the virtual space
 * is set to a tiling mode, and that an existing tree is not present. The window
 * list must also be non-empty !!! */
internal void
CreateWindowTreeForSpaceWithWindows(macos_space *Space, virtual_space *VirtualSpace, std::vector<uint32_t> Windows)
{
    node *New, *Root = CreateRootNode(Windows[0], Space, VirtualSpace);
    VirtualSpace->Tree = Root;

    if(VirtualSpace->Mode == Virtual_Space_Bsp)
    {
        for(size_t Index = 1;
            Index < Windows.size();
            ++Index)
        {
            New = GetFirstMinDepthLeafNode(Root);
            ASSERT(New != NULL);

            node_split Split = (node_split) CVarIntegerValue(CVAR_BSP_SPLIT_MODE);
            if(Split == Split_Optimal)
            {
                Split = OptimalSplitMode(New);
            }

            CreateLeafNodePair(New, New->WindowId, Windows[Index], Split, Space, VirtualSpace);
        }
    }
    else if(VirtualSpace->Mode == Virtual_Space_Monocle)
    {
        for(size_t Index = 1;
            Index < Windows.size();
            ++Index)
        {
            New = CreateRootNode(Windows[Index], Space, VirtualSpace);
            Root->Right = New;
            New->Left = Root;
            Root = New;
        }
    }

    ApplyNodeRegion(VirtualSpace->Tree, VirtualSpace->Mode);
}

/* NOTE(koekeishiya): The caller is responsible for making sure that the space
 * passed to this function is of type kCGSSpaceUser, and that the virtual space
 * is set to bsp tiling mode. The window list must also be non-empty !!! */
internal void
CreateDeserializedWindowTreeForSpaceWithWindows(macos_space *Space, virtual_space *VirtualSpace, std::vector<uint32_t> Windows)
{
    if(!VirtualSpace->Tree)
    {
        char *Buffer = ReadFile(VirtualSpace->TreeLayout);
        if(Buffer)
        {
            VirtualSpace->Tree = DeserializeNodeFromBuffer(Buffer);
            free(Buffer);
        }
        else
        {
            fprintf(stderr, "failed to open '%s' for reading!\n", VirtualSpace->TreeLayout);
            CreateWindowTreeForSpaceWithWindows(Space, VirtualSpace, Windows);
            return;
        }
    }

    node *Root = VirtualSpace->Tree;
    for(size_t Index = 0; Index < Windows.size(); ++Index)
    {
        node *Node = GetFirstMinDepthPseudoLeafNode(Root);
        if(Node)
        {
            if(Node->Parent)
            {
                // NOTE(koekeishiya): This is an intermediate leaf node in the tree.
                // We simulate the process of performing a new split, but use the
                // existing node configuration.
                node_ids NodeIds = AssignNodeIds(Node->Parent->WindowId, Windows[Index]);
                Node->Parent->WindowId = Node_Root;
                Node->Parent->Left->WindowId = NodeIds.Left;
                Node->Parent->Right->WindowId = NodeIds.Right;
            }
            else
            {
                // NOTE(koekeishiya): This is the root node, we temporarily
                // use it as a leaf node, even though it really isn't.
                Node->WindowId = Windows[Index];
            }
        }
        else
        {
            // NOTE(koekeishiya): There are more windows than containers in the layout
            // We perform a regular split with node creation.
            Node = GetFirstMinDepthLeafNode(Root);
            ASSERT(Node != NULL);

            node_split Split = (node_split) CVarIntegerValue(CVAR_BSP_SPLIT_MODE);
            if(Split == Split_Optimal)
            {
                Split = OptimalSplitMode(Node);
            }

            CreateLeafNodePair(Node, Node->WindowId, Windows[Index], Split, Space, VirtualSpace);
        }
    }

    CreateNodeRegion(VirtualSpace->Tree, Region_Full, Space, VirtualSpace);
    CreateNodeRegionRecursive(VirtualSpace->Tree, false, Space, VirtualSpace);
    ApplyNodeRegion(VirtualSpace->Tree, VirtualSpace->Mode, false);
}

void CreateWindowTreeForSpace(macos_space *Space, virtual_space *VirtualSpace)
{
    std::vector<uint32_t> Windows;

    if((VirtualSpace->Tree) ||
       (VirtualSpace->Mode == Virtual_Space_Float))
    {
        return;
    }

    Windows = GetAllVisibleWindowsForSpace(Space);
    if(Windows.empty())
    {
        return;
    }

    CreateWindowTreeForSpaceWithWindows(Space, VirtualSpace, Windows);
}

void CreateDeserializedWindowTreeForSpace(macos_space *Space, virtual_space *VirtualSpace)
{
    if(VirtualSpace->Mode != Virtual_Space_Bsp)
    {
        return;
    }

    std::vector<uint32_t> Windows = GetAllVisibleWindowsForSpace(Space);
    if(Windows.empty())
    {
        return;
    }

    CreateDeserializedWindowTreeForSpaceWithWindows(Space, VirtualSpace, Windows);
}

void CreateWindowTree()
{
    macos_space *Space;
    bool Success = AXLibActiveSpace(&Space);
    ASSERT(Success);

    /* NOTE(koekeishiya): This function appears to always return a valid identifier!
     * Could this potentially return NULL if an invalid CGSSpaceID is passed ?
     * The function returns NULL if "Displays have separate spaces" is disabled !!! */
    CFStringRef DisplayRef = AXLibGetDisplayIdentifierFromSpace(Space->Id);
    ASSERT(DisplayRef);

    if(AXLibIsDisplayChangingSpaces(DisplayRef))
    {
        goto space_free;
    }

    if(Space->Type == kCGSSpaceUser)
    {
        virtual_space *VirtualSpace = AcquireVirtualSpace(Space);
        if(ShouldDeserializeVirtualSpace(VirtualSpace))
        {
            CreateDeserializedWindowTreeForSpace(Space, VirtualSpace);
        }
        else
        {
            CreateWindowTreeForSpace(Space, VirtualSpace);
        }
        ReleaseVirtualSpace(VirtualSpace);
    }

space_free:
    AXLibDestroySpace(Space);
    CFRelease(DisplayRef);
}

/* NOTE(koekeishiya): The caller is responsible for making sure that the space
 * passed to this function is of type kCGSSpaceUser, and that the virtual space
 * is set to a tiling mode, and that an existing tree is present. The window list
 * must also be non-empty !!! */
internal void
RebalanceWindowTreeForSpaceWithWindows(macos_space *Space, virtual_space *VirtualSpace, std::vector<uint32_t> Windows)
{
    std::vector<uint32_t> WindowsInTree = GetAllWindowsInTree(VirtualSpace->Tree, VirtualSpace->Mode);
    std::vector<uint32_t> WindowsToAdd = GetAllWindowsToAddToTree(Windows, WindowsInTree);
    std::vector<uint32_t> WindowsToRemove = GetAllWindowsToRemoveFromTree(Windows, WindowsInTree);

    for(size_t Index = 0;
        Index < WindowsToRemove.size();
        ++Index)
    {
        macos_window *Window = GetWindowByID(WindowsToRemove[Index]);
        if((Window) && (UntileWindowPreValidation(Window)))
        {
            UntileWindowFromSpace(Window, Space, VirtualSpace);
        }
    }

    for(size_t Index = 0;
        Index < WindowsToAdd.size();
        ++Index)
    {
        macos_window *Window = GetWindowByID(WindowsToAdd[Index]);
        if((Window) && (TileWindowPreValidation(Window)))
        {
            TileWindowOnSpace(Window, Space, VirtualSpace);
        }
    }
}

internal void
RebalanceWindowTreeForSpace(macos_space *Space, virtual_space *VirtualSpace)
{
    std::vector<uint32_t> Windows;

    if((!VirtualSpace->Tree) ||
       (VirtualSpace->Mode == Virtual_Space_Float))
    {
        return;
    }

    Windows = GetAllVisibleWindowsForSpace(Space);
    if(Windows.empty())
    {
        return;
    }

    RebalanceWindowTreeForSpaceWithWindows(Space, VirtualSpace, Windows);
}

internal void
RebalanceWindowTree()
{
    macos_space *Space;
    bool Success = AXLibActiveSpace(&Space);
    ASSERT(Success);

    /* NOTE(koekeishiya): This function appears to always return a valid identifier!
     * Could this potentially return NULL if an invalid CGSSpaceID is passed ?
     * The function returns NULL if "Displays have separate spaces" is disabled !!! */
    CFStringRef DisplayRef = AXLibGetDisplayIdentifierFromSpace(Space->Id);
    ASSERT(DisplayRef);

    if(AXLibIsDisplayChangingSpaces(DisplayRef))
    {
        goto space_free;
    }

    if(Space->Type == kCGSSpaceUser)
    {
        virtual_space *VirtualSpace = AcquireVirtualSpace(Space);
        RebalanceWindowTreeForSpace(Space, VirtualSpace);
        ReleaseVirtualSpace(VirtualSpace);
    }

space_free:
    AXLibDestroySpace(Space);
    CFRelease(DisplayRef);
}

void ApplicationLaunchedHandler(void *Data)
{
    macos_application *Application = (macos_application *) Data;

    macos_window **WindowList = AXLibWindowListForApplication(Application);
    if(WindowList)
    {
        macos_window **List = WindowList;
        macos_window *Window;
        while((Window = *List++))
        {
            if(GetWindowByID(Window->Id))
            {
                AXLibDestroyWindow(Window);
            }
            else
            {
                AddWindowToCollection(Window);
                TileWindow(Window);
            }
        }

        free(WindowList);
    }
}

void ApplicationTerminatedHandler(void *Data)
{
    macos_application *Application = (macos_application *) Data;

    RemoveApplication(Application);
    RebalanceWindowTree();
}

void ApplicationHiddenHandler(void *Data)
{
    // macos_application *Application = (macos_application *) Data;
    RebalanceWindowTree();
}

void ApplicationUnhiddenHandler(void *Data)
{
    macos_application *Application = (macos_application *) Data;

    macos_space *Space;
    macos_window *Window, **List, **WindowList;

    bool Success = AXLibActiveSpace(&Space);
    ASSERT(Success);

    if(Space->Type != kCGSSpaceUser)
    {
        goto space_free;
    }

    List = WindowList = AXLibWindowListForApplication(Application);
    if(!WindowList)
    {
        goto space_free;
    }

    while((Window = *List++))
    {
        if((GetWindowByID(Window->Id)) &&
           (AXLibSpaceHasWindow(Space->Id, Window->Id)))
        {
            TileWindow(Window);
        }

        AXLibDestroyWindow(Window);
    }

    free(WindowList);

space_free:
    AXLibDestroySpace(Space);
}

void ApplicationActivatedHandler(void *Data)
{
    macos_application *Application = (macos_application *) Data;
    AXUIElementRef WindowRef = AXLibGetFocusedWindow(Application->Ref);
    if(WindowRef)
    {
        uint32_t WindowId = AXLibGetWindowID(WindowRef);
        CFRelease(WindowRef);

        macos_window *Window = GetWindowByID(WindowId);
        if(Window && IsWindowValid(Window))
        {
            UpdateCVar(CVAR_FOCUSED_WINDOW, (int)Window->Id);
            if(!AXLibHasFlags(Window, Window_Float))
            {
                UpdateCVar(CVAR_BSP_INSERTION_POINT, (int)Window->Id);

                // NOTE(koekeishiya): test global plugin broadcast system.
                int Status = 0;
                ChunkWMBroadcastEvent(PluginName, "focused_window_float", (char *) &Status, sizeof(int));
            }
            else
            {
                // NOTE(koekeishiya): test global plugin broadcast system.
                int Status = 1;
                ChunkWMBroadcastEvent(PluginName, "focused_window_float", (char *) &Status, sizeof(int));
            }
        }
    }
}

void WindowCreatedHandler(void *Data)
{
    macos_window *Window = (macos_window *) Data;

    macos_window *Copy = AXLibCopyWindow(Window);
    AddWindowToCollection(Copy);

    TileWindow(Copy);
}

void WindowDestroyedHandler(void *Data)
{
    macos_window *Window = (macos_window *) Data;

    macos_window *Copy = RemoveWindowFromCollection(Window);
    if(Copy)
    {
        UntileWindow(Copy);
        AXLibDestroyWindow(Copy);
    }
    else
    {
        // NOTE(koekeishiya): Due to unknown reasons, copy returns null for
        // some windows that we receive a destroyed event for, in particular
        // this happens a lot with Steam, what the.. ??
        UntileWindow(Window);
    }
}

void WindowMinimizedHandler(void *Data)
{
    macos_window *Window = (macos_window *) Data;

    macos_window *Copy = GetWindowByID(Window->Id);
    ASSERT(Copy);

    UntileWindow(Copy);
}

void WindowDeminimizedHandler(void *Data)
{
    macos_window *Window = (macos_window *) Data;

    macos_space *Space;
    bool Success = AXLibActiveSpace(&Space);
    ASSERT(Success);

    if((Space->Type == kCGSSpaceUser) &&
       (AXLibSpaceHasWindow(Space->Id, Window->Id)))
    {
        macos_window *Copy = GetWindowByID(Window->Id);
        ASSERT(Copy);

        TileWindow(Copy);
    }

    AXLibDestroySpace(Space);
}

void WindowFocusedHandler(void *Data)
{
    macos_window *Window = (macos_window *) Data;

    macos_window *Copy = GetWindowByID(Window->Id);
    if(Copy && IsWindowValid(Copy))
    {
        UpdateCVar(CVAR_FOCUSED_WINDOW, (int)Copy->Id);
        if(!AXLibHasFlags(Copy, Window_Float))
        {
            UpdateCVar(CVAR_BSP_INSERTION_POINT, (int)Copy->Id);

            // NOTE(koekeishiya): test global plugin broadcast system.
            int Status = 0;
            ChunkWMBroadcastEvent(PluginName, "focused_window_float", &Status, sizeof(int));
        }
        else
        {
            int Status = 1;
            ChunkWMBroadcastEvent(PluginName, "focused_window_float", &Status, sizeof(int));
        }
    }
}

void WindowMovedHandler(void *Data)
{
    macos_window *Window = (macos_window *) Data;

    macos_window *Copy = GetWindowByID(Window->Id);
    if(Copy)
    {
        if(Copy->Position != Window->Position)
        {
            Copy->Position = Window->Position;

            if(CVarIntegerValue(CVAR_WINDOW_REGION_LOCKED))
            {
                ConstrainWindowToRegion(Copy);
            }
        }
    }
}

void WindowResizedHandler(void *Data)
{
    macos_window *Window = (macos_window *) Data;

    macos_window *Copy = GetWindowByID(Window->Id);
    if(Copy)
    {
        if((Copy->Position != Window->Position) ||
           (Copy->Size != Window->Size))
        {
            Copy->Position = Window->Position;
            Copy->Size = Window->Size;

            if(CVarIntegerValue(CVAR_WINDOW_REGION_LOCKED))
            {
                ConstrainWindowToRegion(Copy);
            }
        }
    }
}

void SpaceAndDisplayChangedHandler(void *Data)
{
    UpdateWindowCollection();

    macos_space *Space;
    bool Success = AXLibActiveSpace(&Space);
    ASSERT(Success);

    std::vector<uint32_t> Windows = GetAllVisibleWindowsForSpace(Space);
    if(Space->Type != kCGSSpaceUser)
    {
        goto win_focus;
    }

    unsigned DesktopId, CachedDesktopId;
    Success = AXLibCGSSpaceIDToDesktopID(Space->Id, NULL, &DesktopId);
    ASSERT(Success);

    CachedDesktopId = CVarIntegerValue(CVAR_ACTIVE_DESKTOP);
    if(CachedDesktopId != DesktopId)
    {
        UpdateCVar(CVAR_LAST_ACTIVE_DESKTOP, (int)CachedDesktopId);
        UpdateCVar(CVAR_ACTIVE_DESKTOP, (int)DesktopId);
    }

    if(Windows.empty())
    {
        goto space_free;
    }

    virtual_space *VirtualSpace;
    VirtualSpace = AcquireVirtualSpace(Space);
    if(VirtualSpace->Mode != Virtual_Space_Float)
    {
        if(VirtualSpace->Tree)
        {
            RebalanceWindowTreeForSpaceWithWindows(Space, VirtualSpace, Windows);
        }
        else if(ShouldDeserializeVirtualSpace(VirtualSpace))
        {
            CreateDeserializedWindowTreeForSpaceWithWindows(Space, VirtualSpace, Windows);
        }
        else
        {
            CreateWindowTreeForSpaceWithWindows(Space, VirtualSpace, Windows);
        }
    }
    ReleaseVirtualSpace(VirtualSpace);

win_focus:
    // NOTE(koekeishiya): Update _focused_window to the active window of the new space.
    // This is necessary because the focused notification sometimes fail in these cases.
    // In addition to this, we do not receive the focus notification if this new space
    // is a native fullscreen space.
    if(!Windows.empty())
    {
        UpdateCVar(CVAR_FOCUSED_WINDOW, (int)Windows[0]);
    }

space_free:
    AXLibDestroySpace(Space);
}

/*
 * NOTE(koekeishiya):
 * parameter: const char *Node
 * parameter: void *Data
 * return: bool
 * */
PLUGIN_MAIN_FUNC(PluginMain)
{
    if(StringEquals(Node, "chunkwm_export_application_launched"))
    {
        ApplicationLaunchedHandler(Data);
        return true;
    }
    else if(StringEquals(Node, "chunkwm_export_application_terminated"))
    {
        ApplicationTerminatedHandler(Data);
        return true;
    }
    else if(StringEquals(Node, "chunkwm_export_application_hidden"))
    {
        ApplicationHiddenHandler(Data);
        return true;
    }
    else if(StringEquals(Node, "chunkwm_export_application_unhidden"))
    {
        ApplicationUnhiddenHandler(Data);
        return true;
    }
    else if(StringEquals(Node, "chunkwm_export_application_activated"))
    {
        ApplicationActivatedHandler(Data);
        return true;
    }
    else if(StringEquals(Node, "chunkwm_export_window_created"))
    {
        WindowCreatedHandler(Data);
        return true;
    }
    else if(StringEquals(Node, "chunkwm_export_window_destroyed"))
    {
        WindowDestroyedHandler(Data);
        return true;
    }
    else if(StringEquals(Node, "chunkwm_export_window_minimized"))
    {
        WindowMinimizedHandler(Data);
        return true;
    }
    else if(StringEquals(Node, "chunkwm_export_window_deminimized"))
    {
        WindowDeminimizedHandler(Data);
        return true;
    }
    else if(StringEquals(Node, "chunkwm_export_window_focused"))
    {
        WindowFocusedHandler(Data);
        return true;
    }
    else if(StringEquals(Node, "chunkwm_export_window_moved"))
    {
        WindowMovedHandler(Data);
        return true;
    }
    else if(StringEquals(Node, "chunkwm_export_window_resized"))
    {
        WindowResizedHandler(Data);
        return true;
    }
    else if((StringEquals(Node, "chunkwm_export_space_changed")) ||
            (StringEquals(Node, "chunkwm_export_display_changed")))
    {
        SpaceAndDisplayChangedHandler(Data);
        return true;
    }

    return false;
}

internal bool
Init(plugin_broadcast *ChunkwmBroadcast)
{
    int Port = 4131;
    ChunkWMBroadcastEvent = ChunkwmBroadcast;
    uint32_t ProcessPolicy = Process_Policy_Regular;

    bool Success;
    char *HomeEnv;
    AXUIElementRef ApplicationRef;
    std::vector<macos_application *> Applications;

    Success = BeginCVars();
    if(!Success)
    {
        fprintf(stderr, "   tiling: failed to initialize cvar system!\n");
        goto out;
    }

    CreateCVar(CVAR_SPACE_MODE, Virtual_Space_Bsp);

    CreateCVar(CVAR_SPACE_OFFSET_TOP, 60.0f);
    CreateCVar(CVAR_SPACE_OFFSET_BOTTOM, 50.0f);
    CreateCVar(CVAR_SPACE_OFFSET_LEFT, 50.0f);
    CreateCVar(CVAR_SPACE_OFFSET_RIGHT, 50.0f);
    CreateCVar(CVAR_SPACE_OFFSET_GAP, 20.0f);

    CreateCVar(CVAR_PADDING_STEP_SIZE, 10.0f);
    CreateCVar(CVAR_GAP_STEP_SIZE, 5.0f);

    CreateCVar(CVAR_FOCUSED_WINDOW, 0);
    CreateCVar(CVAR_BSP_INSERTION_POINT, 0);

    CreateCVar(CVAR_ACTIVE_DESKTOP, 0);
    CreateCVar(CVAR_LAST_ACTIVE_DESKTOP, 0);

    CreateCVar(CVAR_BSP_SPAWN_LEFT, 1);
    CreateCVar(CVAR_BSP_OPTIMAL_RATIO, 1.618f);
    CreateCVar(CVAR_BSP_SPLIT_RATIO, 0.5f);
    CreateCVar(CVAR_BSP_SPLIT_MODE, Split_Optimal);

    CreateCVar(CVAR_WINDOW_FOCUS_CYCLE, "none");

    CreateCVar(CVAR_MOUSE_FOLLOWS_FOCUS, 1);

    CreateCVar(CVAR_WINDOW_FLOAT_NEXT, 0);
    CreateCVar(CVAR_WINDOW_FLOAT_CENTER, 0);

    CreateCVar(CVAR_WINDOW_REGION_LOCKED, 0);

    /* NOTE(koekeishiya): The following cvars requires extended dock
     * functionality provided by chwm-sa to work. */

    CreateCVar(CVAR_WINDOW_FLOAT_TOPMOST, 1);

    /*   ---------------------------------------------------------   */

    Success = StartDaemon(Port, DaemonCallback);
    if(!Success)
    {
        fprintf(stderr, "   tiling: could not listen on port %d, abort..\n", Port);
        goto cvar_release;
    }

    HomeEnv = getenv("HOME");
    if(HomeEnv)
    {
        unsigned HomeEnvLength = strlen(HomeEnv);
        unsigned ConfigFileLength = strlen(CONFIG_FILE);
        unsigned PathLength = HomeEnvLength + ConfigFileLength;

        // NOTE(koekeishiya): We don't need to store the config-file, as reloading the config
        // can be done externally by simply executing the bash script instead of sending us
        // a reload command. Stack allocation..
        char PathToConfigFile[PathLength + 1];
        PathToConfigFile[PathLength] = '\0';

        memcpy(PathToConfigFile, HomeEnv, HomeEnvLength);
        memcpy(PathToConfigFile + HomeEnvLength, CONFIG_FILE, ConfigFileLength);

        if(FileExists(PathToConfigFile))
        {
            // NOTE(koekeishiya): The config file is just an executable bash script!
            system(PathToConfigFile);
        }
        else
        {
            fprintf(stderr, "   tiling: config '%s' not found!\n", PathToConfigFile);
        }
    }
    else
    {
        fprintf(stderr,"    tiling: 'env HOME' not set!\n");
    }

    Applications = AXLibRunningProcesses(ProcessPolicy);
    for(size_t Index = 0; Index < Applications.size(); ++Index)
    {
        macos_application *Application = Applications[Index];
        AddApplication(Application);
        AddApplicationWindowList(Application);
    }

    /* NOTE(koekeishiya): Tile windows visible on the current space using configured mode */
    CreateWindowTree();

    /* NOTE(koekeishiya): Set our initial insertion-point on launch. */
    ApplicationRef = AXLibGetFocusedApplication();
    if(ApplicationRef)
    {
        AXUIElementRef WindowRef = AXLibGetFocusedWindow(ApplicationRef);
        CFRelease(ApplicationRef);

        if(WindowRef)
        {
            uint32_t WindowId = AXLibGetWindowID(WindowRef);
            CFRelease(WindowRef);

            macos_window *Window = GetWindowByID(WindowId);
            ASSERT(Window);

            if(IsWindowValid(Window))
            {
                UpdateCVar(CVAR_FOCUSED_WINDOW, (int)Window->Id);
                if(!AXLibHasFlags(Window, Window_Float))
                {
                    UpdateCVar(CVAR_BSP_INSERTION_POINT, (int)Window->Id);
                }
            }
        }
    }

    macos_space *Space;
    Success = AXLibActiveSpace(&Space);
    ASSERT(Success);

    unsigned DesktopId;
    Success = AXLibCGSSpaceIDToDesktopID(Space->Id, NULL, &DesktopId);
    ASSERT(Success);

    AXLibDestroySpace(Space);

    UpdateCVar(CVAR_ACTIVE_DESKTOP, (int)DesktopId);
    UpdateCVar(CVAR_LAST_ACTIVE_DESKTOP, (int)DesktopId);

    Success = BeginVirtualSpaces();
    if(Success)
    {
        goto out;
    }

    fprintf(stderr, "   tiling: failed to initialize virtual space system!\n");

    StopDaemon();
    ClearApplicationCache();
    ClearWindowCache();

cvar_release:
    EndCVars();

out:
    return Success;
}

internal void
Deinit()
{
    StopDaemon();

    ClearApplicationCache();
    ClearWindowCache();

    EndVirtualSpaces();
    EndCVars();
}

/*
 * NOTE(koekeishiya):
 * parameter: plugin_broadcast *Broadcast
 * return: bool -> true if startup succeeded
 */
PLUGIN_BOOL_FUNC(PluginInit)
{
    return Init(Broadcast);
}

PLUGIN_VOID_FUNC(PluginDeInit)
{
    Deinit();
}

// NOTE(koekeishiya): Enable to manually trigger ABI mismatch
#if 0
#undef CHUNKWM_PLUGIN_API_VERSION
#define CHUNKWM_PLUGIN_API_VERSION 0
#endif

// NOTE(koekeishiya): Initialize plugin function pointers.
CHUNKWM_PLUGIN_VTABLE(PluginInit, PluginDeInit, PluginMain)

// NOTE(koekeishiya): Subscribe to ChunkWM events!
chunkwm_plugin_export Subscriptions[] =
{
    chunkwm_export_application_launched,
    chunkwm_export_application_terminated,
    chunkwm_export_application_hidden,
    chunkwm_export_application_unhidden,
    chunkwm_export_application_activated,

    chunkwm_export_window_created,
    chunkwm_export_window_destroyed,
    chunkwm_export_window_minimized,
    chunkwm_export_window_deminimized,
    chunkwm_export_window_focused,

    chunkwm_export_window_moved,
    chunkwm_export_window_resized,

    chunkwm_export_space_changed,
    chunkwm_export_display_changed,
};
CHUNKWM_PLUGIN_SUBSCRIBE(Subscriptions)

// NOTE(koekeishiya): Generate plugin
CHUNKWM_PLUGIN(PluginName, PluginVersion)
