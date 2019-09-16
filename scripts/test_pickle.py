#
#  This file is part of Permafrost Engine. 
#  Copyright (C) 2019 Eduard Permyakov 
#
#  Permafrost Engine is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  Permafrost Engine is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>.
# 
#  Linking this software statically or dynamically with other modules is making 
#  a combined work based on this software. Thus, the terms and conditions of 
#  the GNU General Public License cover the whole combination. 
#  
#  As a special exception, the copyright holders of Permafrost Engine give 
#  you permission to link Permafrost Engine with independent modules to produce 
#  an executable, regardless of the license terms of these independent 
#  modules, and to copy and distribute the resulting executable under 
#  terms of your choice, provided that you also meet, for each linked 
#  independent module, the terms and conditions of the license of that 
#  module. An independent module is a module which is not derived from 
#  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
#  extend this exception to your version of Permafrost Engine, but you are not 
#  obliged to do so. If you do not wish to do so, delete this exception 
#  statement from your version.
#

import pf

def test_pickle_int():

    i1 = -259
    s = pf.pickle_object(i1)
    i2 = pf.unpickle_object(s)
    assert i1 == i2

    i1 = 0x556789
    s = pf.pickle_object(i1)
    i2 = pf.unpickle_object(s)
    assert i1 == i2

    print "Int picking OK!"

def test_pickle_string():
    
    s1 = "Hello World!"
    s = pf.pickle_object(s1)
    s2 = pf.unpickle_object(s)
    assert s1 == s2 

    s1 = ""
    s = pf.pickle_object(s1)
    s2 = pf.unpickle_object(s)
    assert s1 == s2 

    s1 = "\"This is a weird string\"\n"
    s = pf.pickle_object(s1)
    s2 = pf.unpickle_object(s)
    assert s1 == s2 

    print "String pickling OK!"

def test_pickle_tuple():

    t1 = ()
    s = pf.pickle_object(t1)
    t2 = pf.unpickle_object(s)
    assert t1 == t2

    t1 = (0, 1, 2)
    s = pf.pickle_object(t1)
    t2 = pf.unpickle_object(s)
    assert t1 == t2 

    t1 = (0, 1, ("Hello", "World", "!", (2, 3, ())))
    s = pf.pickle_object(t1)
    t2 = pf.unpickle_object(s)
    assert t1 == t2 

    #Self referencing
    l = []
    t1 = (1, 2, l)
    l.append(t1)
    s = pf.pickle_object(t1)
    t2 = pf.unpickle_object(s)
    assert t1.__repr__() == t2.__repr__() # Can't compare direcly due to recursive def
    assert id(t2) == id(t2[2][0])

    a = 1
    t1 = (a, a, a)
    s = pf.pickle_object(t1)
    t2 = pf.unpickle_object(s)
    assert t1 == t2
    assert id(t2[0]) == id(t2[1])
    assert id(t2[1]) == id(t2[2])

    print "Tuple pickling OK!"

def test_pickle_list():

    l1 = []
    s = pf.pickle_object(l1)
    l2 = pf.unpickle_object(s)
    assert l1 == l2

    l1 = [1, 2, 3, "Hello", "World", (1, 2), [3], [[[9]]], "!", (1, [120])]
    s = pf.pickle_object(l1)
    l2 = pf.unpickle_object(s)
    assert l1 == l2

    #self referencing
    l1 = []
    l1.append(l1)
    s = pf.pickle_object(l1)
    l2 = pf.unpickle_object(s)
    assert l1.__repr__() == l2.__repr__() # Can't compare direcly due to recursive def
    assert id(l2) == id(l2[0])

    print "List pickling OK!"

def test_pickle_dict():

    d1 = {}
    s = pf.pickle_object(d1)
    d2 = pf.unpickle_object(s)
    assert d1 == d2

    a = "same",
    d1 = {
        "Hello" : "World",
        "Apple" : "Orange",
        a       : a,
        99      : 12356,
        100     : (1, 2, 3),
        101     : {0 : {}, 1 : {}}
    }
    s = pf.pickle_object(d1)
    d2 = pf.unpickle_object(s)
    assert d1 == d2

    #self referencing
    d1 = {}
    d1["key"] = d1
    s = pf.pickle_object(d1)
    d2 = pf.unpickle_object(s)
    assert d1.__repr__() == d2.__repr__()
    assert id(d2) == id(d2["key"])

    print "Dict pickling OK!"

try:
    test_pickle_int()
    test_pickle_string()
    test_pickle_tuple()
    test_pickle_list()
    test_pickle_dict()
finally:
    pf.global_event(pf.SDL_QUIT, None)

