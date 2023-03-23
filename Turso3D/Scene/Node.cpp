// For conditions of distribution and use, see copyright notice in License.txt

#include "../IO/Log.h"
#include "../IO/Stream.h"
#include "../Object/Allocator.h"
#include "../Object/ObjectResolver.h"
#include "../Resource/JSONFile.h"
#include "Scene.h"

static std::vector<SharedPtr<Node> > noChildren;
std::vector<NodeInfo> Node::nodeInfos;
std::vector<LocalTransform> Node::localTransforms;
std::vector<Matrix3x4> Node::worldTransforms;
std::vector<Node*> Node::indexToNode;

Node::Node() :
    parent(nullptr),
    flags(NF_ENABLED),
    layer(LAYER_DEFAULT)
{
    // Reserve space in node structures
    arrayIdx = (unsigned)nodeInfos.size();

    nodeInfos.resize(arrayIdx + 1);
    localTransforms.resize(arrayIdx + 1);
    worldTransforms.resize(arrayIdx + 1);
    indexToNode.push_back(this);

    nodeInfos[arrayIdx].scene = nullptr;
    nodeInfos[arrayIdx].id = 0;
}

Node::~Node()
{
    RemoveAllChildren();

    // At the time of destruction the node should not have a parent, or be in a scene
    assert(!parent);
    assert(!ParentScene());

    // Free up space in node structures. Swap with last if needed
    size_t newSize = nodeInfos.size() - 1;

    if (arrayIdx != newSize)
    {
        std::swap(nodeInfos[arrayIdx], nodeInfos[newSize]);
        std::swap(localTransforms[arrayIdx], localTransforms[newSize]);
        std::swap(worldTransforms[arrayIdx], worldTransforms[newSize]);
        std::swap(indexToNode[arrayIdx], indexToNode[newSize]);
        indexToNode[arrayIdx]->arrayIdx = arrayIdx;
    }

    nodeInfos.resize(newSize);
    localTransforms.resize(newSize);
    worldTransforms.resize(newSize);
    indexToNode.resize(newSize);
}

void Node::RegisterObject()
{
    RegisterFactory<Node>();
    RegisterRefAttribute("name", &Node::Name, &Node::SetName);
    RegisterAttribute("enabled", &Node::IsEnabled, &Node::SetEnabled, true);
    RegisterAttribute("temporary", &Node::IsTemporary, &Node::SetTemporary, false);
    RegisterAttribute("layer", &Node::Layer, &Node::SetLayer, LAYER_DEFAULT);
}

void Node::Load(Stream& source, ObjectResolver& resolver)
{
    // Load child nodes before own attributes to enable e.g. AnimatedModel to set bones at load time
    size_t numChildren = source.ReadVLE();
    children.reserve(numChildren);

    for (size_t i = 0; i < numChildren; ++i)
    {
        StringHash childType(source.Read<StringHash>());
        unsigned childId = source.Read<unsigned>();
        Node* child = CreateChild(childType);
        if (child)
        {
            resolver.StoreObject(childId, child);
            child->Load(source, resolver);
        }
        else
        {
            // If child is unknown type, skip all its attributes and children
            SkipHierarchy(source);
        }
    }

    // Type and id has been read by the parent
    Serializable::Load(source, resolver);
}

void Node::Save(Stream& dest)
{
    // Write type and ID first, followed by child nodes and attributes
    dest.Write(Type());
    dest.Write(Id());
    dest.WriteVLE(NumPersistentChildren());

    for (auto it = children.begin(); it != children.end(); ++it)
    {
        Node* child = *it;
        if (!child->IsTemporary())
            child->Save(dest);
    }

    Serializable::Save(dest);
}

void Node::LoadJSON(const JSONValue& source, ObjectResolver& resolver)
{
    const JSONArray& childArray = source["children"].GetArray();
    children.reserve(childArray.size());

    for (auto it = childArray.begin(); it != childArray.end(); ++it)
    {
        const JSONValue& childJSON = *it;
        StringHash childType(childJSON["type"].GetString());
        unsigned childId = (unsigned)childJSON["id"].GetNumber();
        Node* child = CreateChild(childType);
        if (child)
        {
            resolver.StoreObject(childId, child);
            child->LoadJSON(childJSON, resolver);
        }
    }

    // Type and id has been read by the parent
    Serializable::LoadJSON(source, resolver);
}

void Node::SaveJSON(JSONValue& dest)
{
    dest["type"] = TypeName();
    dest["id"] = Id();

    if (NumPersistentChildren())
    {
        JSONValue& childArray = dest["children"];
        for (auto it = children.begin(); it != children.end(); ++it)
        {
            Node* child = *it;
            if (!child->IsTemporary())
            {
                JSONValue childJSON;
                child->SaveJSON(childJSON);
                childArray.Push(childJSON);
            }
        }
    }

    Serializable::SaveJSON(dest);
}

bool Node::SaveJSON(Stream& dest)
{
    JSONFile json;
    SaveJSON(json.Root());
    return json.Save(dest);
}

void Node::SetName(const std::string& newName)
{
    nodeInfos[arrayIdx].name = newName;
    nodeInfos[arrayIdx].nameHash = StringHash(newName);
}

void Node::SetName(const char* newName)
{
    nodeInfos[arrayIdx].name = newName;
    nodeInfos[arrayIdx].nameHash = StringHash(newName);
}

void Node::SetLayer(unsigned char newLayer)
{
    if (layer < 32)
        layer = newLayer;
    else
        LOGERROR("Can not set layer 32 or higher");
}

void Node::SetEnabled(bool enable)
{
    if (enable != TestFlag(NF_ENABLED))
    {
        SetFlag(NF_ENABLED, enable);
        OnEnabledChanged(enable);
    }
}

void Node::SetEnabledRecursive(bool enable)
{
    SetEnabled(enable);
    for (auto it = children.begin(); it != children.end(); ++it)
    {
        Node* child = *it;
        child->SetEnabledRecursive(enable);
    }
}

void Node::SetTemporary(bool enable)
{
    SetFlag(NF_TEMPORARY, enable);
}

void Node::SetParent(Node* newParent)
{
    if (newParent)
        newParent->AddChild(this);
    else if (parent)
        parent->RemoveChild(this);
}

Node* Node::CreateChild(StringHash childType)
{
    SharedPtr<Object> newObject = Create(childType);
    if (!newObject)
    {
        LOGERROR("Could not create child node of unknown type " + childType.ToString());
        return nullptr;
    }
    Node* child = dynamic_cast<Node*>(newObject.Get());
    if (!child)
    {
        LOGERROR(newObject->TypeName() + " is not a Node subclass, could not add as a child");
        return nullptr;
    }

    AddChild(child);
    return child;
}

Node* Node::CreateChild(StringHash childType, const std::string& childName)
{
    return CreateChild(childType, childName.c_str());
}

Node* Node::CreateChild(StringHash childType, const char* childName)
{
    Node* child = CreateChild(childType);
    if (child)
        child->SetName(childName);
    return child;
}

void Node::AddChild(Node* child)
{
    // Check for illegal or redundant parent assignment
    if (!child || child->parent == this)
        return;
    
#ifdef _DEBUG
    // Check for possible illegal or cyclic parent assignment
    if (child == this)
    {
        LOGERROR("Attempted parenting node to self");
        return;
    }

    Node* current = parent;
    while (current)
    {
        if (current == child)
        {
            LOGERROR("Attempted cyclic node parenting");
            return;
        }
        current = current->parent;
    }
#endif

    Node* oldParent = child->parent;
    if (oldParent)
    {
        for (auto it = oldParent->children.begin(); it != oldParent->children.end(); ++it)
        {
            if (*it == child)
            {
                oldParent->children.erase(it);
                break;
            }
        }
    }

    children.push_back(child);
    child->parent = this;
    child->OnParentSet(this, oldParent);

    Scene* scene = ParentScene();
    if (scene)
        scene->AddNode(child);
}

void Node::RemoveChild(Node* child)
{
    if (!child || child->parent != this)
        return;

    for (size_t i = 0; i < children.size(); ++i)
    {
        if (children[i] == child)
        {
            RemoveChild(i);
            break;
        }
    }
}

void Node::RemoveChild(size_t index)
{
    if (index >= children.size())
        return;

    Node* child = children[index];
    // Detach from both the parent and the scene (removes id assignment)
    child->parent = nullptr;
    child->SetFlag(NF_SPATIAL_PARENT, false);

    Scene* scene = ParentScene();
    if (scene)
        scene->RemoveNode(child);

    children.erase(children.begin() + index);
}

void Node::RemoveAllChildren()
{
    Scene* scene = ParentScene();

    // Remove in reverse order to limit node structure swaps
    for (size_t i = children.size() - 1; i < children.size(); --i)
    {
        Node* child = children[i];
        child->parent = nullptr;
        child->SetFlag(NF_SPATIAL_PARENT, false);
        
        if (scene)
            scene->RemoveNode(child);

        children[i].Reset();
    }

    children.clear();
}

void Node::RemoveSelf()
{
    if (parent)
        parent->RemoveChild(this);
}

size_t Node::NumPersistentChildren() const
{
    size_t ret = 0;

    for (auto it = children.begin(); it != children.end(); ++it)
    {
        Node* child = *it;
        if (!child->IsTemporary())
            ++ret;
    }

    return ret;
}

void Node::FindAllChildren(std::vector<Node*>& result) const
{
    for (auto it = children.begin(); it != children.end(); ++it)
    {
        Node* child = *it;
        result.push_back(child);
        child->FindAllChildren(result);
    }
}

Node* Node::FindChild(const std::string& childName, bool recursive) const
{
    return FindChild(childName.c_str(), recursive);
}

Node* Node::FindChild(const char* childName, bool recursive) const
{
    for (auto it = children.begin(); it != children.end(); ++it)
    {
        Node* child = *it;
        if (child->Name() == childName)
            return child;
        else if (recursive && child->children.size())
        {
            Node* childResult = child->FindChild(childName, recursive);
            if (childResult)
                return childResult;
        }
    }

    return nullptr;
}

Node* Node::FindChild(StringHash childNameHash, bool recursive) const
{
    for (auto it = children.begin(); it != children.end(); ++it)
    {
        Node* child = *it;
        if (child->NameHash() == childNameHash)
            return child;
        else if (recursive && child->children.size())
        {
            Node* childResult = child->FindChild(childNameHash, recursive);
            if (childResult)
                return childResult;
        }
    }

    return nullptr;
}

Node* Node::FindChildOfType(StringHash childType, bool recursive) const
{
    for (auto it = children.begin(); it != children.end(); ++it)
    {
        Node* child = *it;
        if (child->Type() == childType || DerivedFrom(child->Type(), childType))
            return child;
        else if (recursive && child->children.size())
        {
            Node* childResult = child->FindChild(childType, recursive);
            if (childResult)
                return childResult;
        }
    }

    return nullptr;
}

Node* Node::FindChildOfType(StringHash childType, const std::string& childName, bool recursive) const
{
    return FindChildOfType(childType, childName.c_str(), recursive);
}

Node* Node::FindChildOfType(StringHash childType, const char* childName, bool recursive) const
{
    for (auto it = children.begin(); it != children.end(); ++it)
    {
        Node* child = *it;
        if ((child->Type() == childType || DerivedFrom(child->Type(), childType)) && child->Name() == childName)
            return child;
        else if (recursive && child->children.size())
        {
            Node* childResult = child->FindChildOfType(childType, childName, recursive);
            if (childResult)
                return childResult;
        }
    }

    return nullptr;
}

Node* Node::FindChildOfType(StringHash childType, StringHash childNameHash, bool recursive) const
{
    for (auto it = children.begin(); it != children.end(); ++it)
    {
        Node* child = *it;
        if ((child->Type() == childType || DerivedFrom(child->Type(), childType)) && child->NameHash() == childNameHash)
            return child;
        else if (recursive && child->children.size())
        {
            Node* childResult = child->FindChildOfType(childType, childNameHash, recursive);
            if (childResult)
                return childResult;
        }
    }

    return nullptr;
}
Node* Node::FindChildByLayer(unsigned layerMask, bool recursive) const
{
    for (auto it = children.begin(); it != children.end(); ++it)
    {
        Node* child = *it;
        if (child->LayerMask() && layerMask)
            return child;
        else if (recursive && child->children.size())
        {
            Node* childResult = child->FindChildByLayer(layerMask, recursive);
            if (childResult)
                return childResult;
        }
    }

    return nullptr;
}

void Node::FindChildren(std::vector<Node*>& result, StringHash childType, bool recursive) const
{
    for (auto it = children.begin(); it != children.end(); ++it)
    {
        Node* child = *it;
        if (child->Type() == childType || DerivedFrom(child->Type(), childType))
            result.push_back(child);
        if (recursive && child->children.size())
            child->FindChildren(result, childType, recursive);
    }
}

void Node::FindChildrenByLayer(std::vector<Node*>& result, unsigned layerMask, bool recursive) const
{
    for (auto it = children.begin(); it != children.end(); ++it)
    {
        Node* child = *it;
        if (child->LayerMask() & layerMask)
            result.push_back(child);
        if (recursive && child->children.size())
            child->FindChildrenByLayer(result, layerMask, recursive);
    }
}

void Node::SetScene(Scene* newScene)
{
    Scene* oldScene = nodeInfos[arrayIdx].scene;
    nodeInfos[arrayIdx].scene = newScene;
    OnSceneSet(newScene, oldScene);
}

void Node::SetId(unsigned newId)
{
    nodeInfos[arrayIdx].id = newId;
}

void Node::SkipHierarchy(Stream& source)
{
    Serializable::Skip(source);

    size_t numChildren = source.ReadVLE();
    for (size_t i = 0; i < numChildren; ++i)
    {
        source.Read<StringHash>(); // StringHash childType
        source.Read<unsigned>(); // unsigned childId
        SkipHierarchy(source);
    }
}

void Node::OnParentSet(Node*, Node*)
{
}

void Node::OnSceneSet(Scene*, Scene*)
{
}

void Node::OnEnabledChanged(bool)
{
}

