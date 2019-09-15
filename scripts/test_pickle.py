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

    print "Tuple pickling OK!"

test_pickle_int()
test_pickle_string()
test_pickle_tuple()

pf.global_event(pf.SDL_QUIT, None)

