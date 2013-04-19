#ifndef Magnum_SceneGraph_Object_hpp
#define Magnum_SceneGraph_Object_hpp
/*
    This file is part of Magnum.

    Copyright © 2010, 2011, 2012, 2013 Vladimír Vondruš <mosra@centrum.cz>

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
*/

/** @file
 * @brief @ref compilation-speedup-hpp "Template implementation" for Object.h
 */

#include "AbstractTransformation.h"
#include "Object.h"

#include <algorithm>
#include <stack>

#include "Scene.h"

namespace Magnum { namespace SceneGraph {

template<UnsignedInt dimensions, class T> AbstractObject<dimensions, T>::AbstractObject() {}
template<UnsignedInt dimensions, class T> AbstractObject<dimensions, T>::~AbstractObject() {}

template<UnsignedInt dimensions, class T> inline AbstractTransformation<dimensions, T>::AbstractTransformation() {}
template<UnsignedInt dimensions, class T> inline AbstractTransformation<dimensions, T>::~AbstractTransformation() {}

template<class Transformation> Scene<Transformation>* Object<Transformation>::scene() {
    return static_cast<Scene<Transformation>*>(sceneObject());
}

template<class Transformation> const Scene<Transformation>* Object<Transformation>::scene() const {
    return static_cast<const Scene<Transformation>*>(sceneObject());
}

template<class Transformation> Object<Transformation>* Object<Transformation>::sceneObject() {
    Object<Transformation>* p(this);
    while(p && !p->isScene()) p = p->parent();
    return p;
}

template<class Transformation> const Object<Transformation>* Object<Transformation>::sceneObject() const {
    const Object<Transformation>* p(this);
    while(p && !p->isScene()) p = p->parent();
    return p;
}

template<class Transformation> Object<Transformation>* Object<Transformation>::setParent(Object<Transformation>* parent) {
    /* Skip if parent is already parent or this is scene (which cannot have parent) */
    /** @todo Assert for setting parent to scene */
    if(this->parent() == parent || isScene()) return this;

    /* Object cannot be parented to its child */
    Object<Transformation>* p = parent;
    while(p) {
        /** @todo Assert for this */
        if(p == this) return this;
        p = p->parent();
    }

    /* Remove the object from old parent children list */
    if(this->parent()) this->parent()->Corrade::Containers::template LinkedList<Object<Transformation>>::cut(this);

    /* Add the object to list of new parent */
    if(parent) parent->Corrade::Containers::LinkedList<Object<Transformation>>::insert(this);

    setDirty();
    return this;
}

template<class Transformation> typename Transformation::DataType Object<Transformation>::absoluteTransformation() const {
    if(!parent()) return Transformation::transformation();
    return Transformation::compose(parent()->absoluteTransformation(), Transformation::transformation());
}

template<class Transformation> void Object<Transformation>::setDirty() {
    /* The transformation of this object (and all children) is already dirty,
       nothing to do */
    if(flags & Flag::Dirty) return;

    Object<Transformation>* self = static_cast<Object<Transformation>*>(this);

    /* Make all features dirty */
    for(AbstractFeature<Transformation::Dimensions, typename Transformation::Type>* i = self->firstFeature(); i; i = i->nextFeature())
        i->markDirty();

    /* Make all children dirty */
    for(Object<Transformation>* i = self->firstChild(); i; i = i->nextSibling())
        i->setDirty();

    /* Mark object as dirty */
    flags |= Flag::Dirty;
}

template<class Transformation> void Object<Transformation>::setClean() {
    /* The object (and all its parents) are already clean, nothing to do */
    if(!(flags & Flag::Dirty)) return;

    /* Collect all parents, compute base transformation */
    std::stack<Object<Transformation>*> objects;
    typename Transformation::DataType absoluteTransformation;
    Object<Transformation>* p = static_cast<Object<Transformation>*>(this);
    for(;;) {
        objects.push(p);

        p = p->parent();

        /* On root object, base transformation is identity */
        if(!p) break;

        /* Parent object is clean, base transformation is its absolute
           transformation */
        if(!p->isDirty()) {
            absoluteTransformation = p->absoluteTransformation();
            break;
        }
    }

    /* Clean features on every collected object, going down from root object */
    while(!objects.empty()) {
        Object<Transformation>* o = objects.top();
        objects.pop();

        /* Compose transformation and clean object */
        absoluteTransformation = Transformation::compose(absoluteTransformation, o->transformation());
        CORRADE_INTERNAL_ASSERT(o->isDirty());
        o->setClean(absoluteTransformation);
        CORRADE_ASSERT(!o->isDirty(), "SceneGraph::Object::setClean(): original implementation was not called", );
    }
}

template<class Transformation> std::vector<typename DimensionTraits<Transformation::Dimensions, typename Transformation::Type>::MatrixType> Object<Transformation>::transformationMatrices(const std::vector<AbstractObject<Transformation::Dimensions, typename Transformation::Type>*>& objects, const typename DimensionTraits<Transformation::Dimensions, typename Transformation::Type>::MatrixType& initialTransformationMatrix) const {
    std::vector<Object<Transformation>*> castObjects(objects.size());
    for(std::size_t i = 0; i != objects.size(); ++i)
        /** @todo Ensure this doesn't crash, somehow */
        castObjects[i] = static_cast<Object<Transformation>*>(objects[i]);

    std::vector<typename Transformation::DataType> transformations = this->transformations(std::move(castObjects), Transformation::fromMatrix(initialTransformationMatrix));
    std::vector<typename DimensionTraits<Transformation::Dimensions, typename Transformation::Type>::MatrixType> transformationMatrices(transformations.size());
    for(std::size_t i = 0; i != objects.size(); ++i)
        transformationMatrices[i] = Transformation::toMatrix(transformations[i]);

    return transformationMatrices;
}

/*
Computing absolute transformations for given list of objects

The goal is to compute absolute transformation only once for each object
involved. Objects contained in the subtree specified by `object` list are
divided into two groups:
 - "joints", which are either part of `object` list or they have more than one
   child in the subtree
 - "non-joints", i.e. paths between joints

Then for all joints their transformation (relative to parent joint) is
computed and recursively concatenated together. Resulting transformations for
joints which were originally in `object` list is then returned.
*/
template<class Transformation> std::vector<typename Transformation::DataType> Object<Transformation>::transformations(std::vector<Object<Transformation>*> objects, const typename Transformation::DataType& initialTransformation) const {
    CORRADE_ASSERT(objects.size() < 0xFFFFu, "SceneGraph::Object::transformations(): too large scene", {});

    /* Remember object count for later */
    std::size_t objectCount = objects.size();

    /* Mark all original objects as joints and create initial list of joints
       from them */
    for(std::size_t i = 0; i != objects.size(); ++i) {
        /* Multiple occurences of one object in the array, don't overwrite it
           with different counter */
        if(objects[i]->counter != 0xFFFFu) continue;

        objects[i]->counter = i;
        objects[i]->flags |= Flag::Joint;
    }
    std::vector<Object<Transformation>*> jointObjects(objects);

    /* Scene object */
    const Scene<Transformation>* scene = this->scene();

    /* Nearest common ancestor not yet implemented - assert this is done on scene */
    CORRADE_ASSERT(scene == this, "SceneGraph::Object::transformationMatrices(): currently implemented only for Scene", {});

    /* Mark all objects up the hierarchy as visited */
    auto it = objects.begin();
    while(!objects.empty()) {
        /* Already visited, remove and continue to next (duplicate occurence) */
        if((*it)->flags & Flag::Visited) {
            it = objects.erase(it);
            continue;
        }

        /* Mark the object as visited */
        (*it)->flags |= Flag::Visited;

        Object<Transformation>* parent = (*it)->parent();

        /* If this is root object, remove from list */
        if(!parent) {
            CORRADE_ASSERT(*it == scene, "SceneGraph::Object::transformations(): the objects are not part of the same tree", {});
            it = objects.erase(it);

        /* Parent is an joint or already visited - remove current from list */
        } else if(parent->flags & (Flag::Visited|Flag::Joint)) {
            it = objects.erase(it);

            /* If not already marked as joint, mark it as such and add it to
               list of joint objects */
            if(!(parent->flags & Flag::Joint)) {
                CORRADE_ASSERT(jointObjects.size() < 0xFFFFu,
                               "SceneGraph::Object::transformations(): too large scene", {});
                CORRADE_INTERNAL_ASSERT(parent->counter == 0xFFFFu);
                parent->counter = jointObjects.size();
                parent->flags |= Flag::Joint;
                jointObjects.push_back(parent);
            }

        /* Else go up the hierarchy */
        } else *it = parent;

        /* Cycle if reached end */
        if(it == objects.end()) it = objects.begin();
    }

    /* Array of absolute transformations in joints */
    std::vector<typename Transformation::DataType> jointTransformations(jointObjects.size());

    /* Compute transformations for all joints */
    for(std::size_t i = 0; i != jointTransformations.size(); ++i)
        computeJointTransformation(jointObjects, jointTransformations, i, initialTransformation);

    /* Copy transformation for second or next occurences from first occurence
       of duplicate object */
    for(std::size_t i = 0; i != objectCount; ++i) {
        if(jointObjects[i]->counter != i)
            jointTransformations[i] = jointTransformations[jointObjects[i]->counter];
    }

    /* All visited marks are now cleaned, clean joint marks and counters */
    for(auto i: jointObjects) {
        /* All not-already cleaned objects (...duplicate occurences) should
           have joint mark */
        CORRADE_INTERNAL_ASSERT(i->counter = 0xFFFFu || i->flags & Flag::Joint);
        i->flags &= ~Flag::Joint;
        i->counter = 0xFFFFu;
    }

    /* Shrink the array to contain only transformations of requested objects and return */
    jointTransformations.resize(objectCount);
    return jointTransformations;
}

template<class Transformation> typename Transformation::DataType Object<Transformation>::computeJointTransformation(const std::vector<Object<Transformation>*>& jointObjects, std::vector<typename Transformation::DataType>& jointTransformations, const std::size_t joint, const typename Transformation::DataType& initialTransformation) const {
    Object<Transformation>* o = jointObjects[joint];

    /* Transformation already computed ("unvisited" by this function before
       either due to recursion or duplicate object occurences), done */
    if(!(o->flags & Flag::Visited)) return jointTransformations[joint];

    /* Initialize transformation */
    jointTransformations[joint] = o->transformation();

    /* Go up until next joint or root */
    for(;;) {
        /* Clean visited mark */
        CORRADE_INTERNAL_ASSERT(o->flags & Flag::Visited);
        o->flags &= ~Flag::Visited;

        Object<Transformation>* parent = o->parent();

        /* Root object, compose transformation with initial, done */
        if(!parent) {
            CORRADE_INTERNAL_ASSERT(o->isScene());
            return (jointTransformations[joint] =
                Transformation::compose(initialTransformation, jointTransformations[joint]));

        /* Joint object, compose transformation with the joint, done */
        } else if(parent->flags & Flag::Joint) {
            return (jointTransformations[joint] =
                Transformation::compose(computeJointTransformation(jointObjects, jointTransformations, parent->counter, initialTransformation), jointTransformations[joint]));

        /* Else compose transformation with parent, go up the hierarchy */
        } else {
            jointTransformations[joint] = Transformation::compose(parent->transformation(), jointTransformations[joint]);
            o = parent;
        }
    }
}

template<class Transformation> void Object<Transformation>::setClean(const std::vector<AbstractObject<Transformation::Dimensions, typename Transformation::Type>*>& objects) const {
    std::vector<Object<Transformation>*> castObjects(objects.size());
    for(std::size_t i = 0; i != objects.size(); ++i)
        /** @todo Ensure this doesn't crash, somehow */
        castObjects[i] = static_cast<Object<Transformation>*>(objects[i]);

    setClean(std::move(castObjects));
}

template<class Transformation> void Object<Transformation>::setClean(std::vector<Object<Transformation>*> objects) {
    /* Remove all clean objects from the list */
    auto firstClean = std::remove_if(objects.begin(), objects.end(), [](Object<Transformation>* o) { return !o->isDirty(); });
    objects.erase(firstClean, objects.end());

    /* No dirty objects left, done */
    if(objects.empty()) return;

    /* Add non-clean parents to the list. Mark each added object as visited, so
       they aren't added more than once */
    for(std::size_t end = objects.size(), i = 0; i != end; ++i) {
        Object<Transformation>* o = objects[i];
        o->flags |= Flag::Visited;

        Object<Transformation>* parent = o->parent();
        while(parent && !(parent->flags & Flag::Visited) && parent->isDirty()) {
            objects.push_back(parent);
            parent = parent->parent();
        }
    }

    /* Cleanup all marks */
    for(auto o: objects) o->flags &= ~Flag::Visited;

    /* Compute absolute transformations */
    Scene<Transformation>* scene = objects[0]->scene();
    CORRADE_ASSERT(scene, "Object::setClean(): objects must be part of some scene", );
    std::vector<typename Transformation::DataType> transformations(scene->transformations(objects));

    /* Go through all objects and clean them */
    for(std::size_t i = 0; i != objects.size(); ++i) {
        /* The object might be duplicated in the list, don't clean it more than once */
        if(!objects[i]->isDirty()) continue;

        objects[i]->setClean(transformations[i]);
        CORRADE_ASSERT(!objects[i]->isDirty(), "SceneGraph::Object::setClean(): original implementation was not called", );
    }
}

template<class Transformation> void Object<Transformation>::setClean(const typename Transformation::DataType& absoluteTransformation) {
    /* "Lazy storage" for transformation matrix and inverted transformation matrix */
    typedef typename AbstractFeature<Transformation::Dimensions, typename Transformation::Type>::CachedTransformation CachedTransformation;
    typename AbstractFeature<Transformation::Dimensions, typename Transformation::Type>::CachedTransformations cached;
    typename DimensionTraits<Transformation::Dimensions, typename Transformation::Type>::MatrixType
        matrix, invertedMatrix;

    /* Clean all features */
    for(AbstractFeature<Transformation::Dimensions, typename Transformation::Type>* i = this->firstFeature(); i; i = i->nextFeature()) {
        /* Cached absolute transformation, compute it if it wasn't
            computed already */
        if(i->cachedTransformations() & CachedTransformation::Absolute) {
            if(!(cached & CachedTransformation::Absolute)) {
                cached |= CachedTransformation::Absolute;
                matrix = Transformation::toMatrix(absoluteTransformation);
            }

            i->clean(matrix);
        }

        /* Cached inverse absolute transformation, compute it if it wasn't
            computed already */
        if(i->cachedTransformations() & CachedTransformation::InvertedAbsolute) {
            if(!(cached & CachedTransformation::InvertedAbsolute)) {
                cached |= CachedTransformation::InvertedAbsolute;
                invertedMatrix = Transformation::toMatrix(Transformation::inverted(absoluteTransformation));
            }

            i->cleanInverted(invertedMatrix);
        }
    }

    /* Mark object as clean */
    flags &= ~Flag::Dirty;
}

}}

#endif
