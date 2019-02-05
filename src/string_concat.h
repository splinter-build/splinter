// Copyright 2017 Michael Jones All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NINJA_STRING_CONCAT_H_
#define NINJA_STRING_CONCAT_H_

#include <string>
#include <tuple>
#include <string_view>

/**
 * Gets a pointer to the underlying data of a string type object
 *
 * If the parameter is a char *, or a char[], then this
 * function simply returns the parameter.
 *
 * If the parameter is neither a char* or char[], then return
 * the result of calling the parameters .data() function.
 *
 * If the parameter has no .data() function, a compile error results.
 *
 * The returned pointer provides no guarantee on the string being nul
 * terminated. Always use string_data() with string_size() to ensure
 * no out of bounds data access occurs.
 */
template<class STRING_T>
constexpr decltype(auto) string_data(const STRING_T& str)
{
    if constexpr(std::is_pointer_v<STRING_T>)
    {
        return str;
    }
    else
    {
        return std::data(str);
    }
} // string_data()


//-----------------------------------------------------------------------------

/**
 * Gets the size of the string provided as the parameter
 *
 * If the parameter is a char *, or a char[], then this
 * function calls the string length function, and therefore
 * the provided string must be nul-terminated.
 *
 * (Future enhancement will allow the size of char[] parameters
 *  to be statically determined at compile time)
 *
 * If the parameter is neither a char* or char[], then return
 * the result of calling std::size().
 */
template<class STRING_T>
constexpr size_t string_size(const STRING_T & str)
{
    if constexpr(std::is_pointer_v<STRING_T>)
    {
        return std::char_traits<std::remove_pointer_t<STRING_T>>::length(str);
    }
    else if constexpr(std::is_array_v<STRING_T>)
    {
        // TODO: Throw if the array contains an embedded nul character.
        // TODO: Return the static size based on template deduction.
        return std::char_traits<std::remove_pointer_t<std::decay_t<STRING_T>>>::length(str);
    }
    else
    {
        return std::size(str);
    }
} // string_size()


//-----------------------------------------------------------------------------

/**
 * Effecienctly appends multiple strings together using, at most, a single
 * allocation to reserve the necessary space, via the "reserve" function call.
 *
 * \arg dest   -- The destination where all of the appended strings will be stored
 * \arg others -- The strings to append into the destination.
 *
 * \returns Nothing.
 */
template<typename DESTINATION_T, typename ... STRS_T>
constexpr void string_append(DESTINATION_T & dest, const STRS_T& ... others)
{
    // C++17 fold for summation
    dest.reserve( ( string_size(dest) + ... + string_size(others) ) );

    // C++17 fold for function calls.
    ( dest.append(string_data(others), string_size(others)), ... );
} // string_append()


//-----------------------------------------------------------------------------

/**
 * Concatinates all of the provided string-like objects into a single
 * string of the type RESULT_T. RESULT_T must have reserve() and append()
 * functions.
 *
 * This operation uses a single allocation to acquire storage, via the "reserve" function call.
 *
 * \arg string -- the strings to concatinate together.
 *
 * \returns A RESULT_T object containing the strings concatinated together.
 */
template<typename RESULT_T, typename ... STRS_T>
constexpr RESULT_T basic_string_concat(const STRS_T& ... strings)
{
    RESULT_T result;
    string_append(result, strings...);
    return result;
} // basic_string_concat()


//-----------------------------------------------------------------------------

/**
 * Concatinates all of the provided string-like objects into a single std::string.
 *
 * This operation uses a single allocation to acquire storage, via the "reserve" function call.
 *
 * \arg string -- the strings to concatinate together.
 *
 * \returns A std::string object containing the strings concatinated together.
 */
template<typename ... STRS_T>
constexpr decltype(auto) string_concat(STRS_T && ... strs)
{
    return basic_string_concat<std::string>(std::forward<STRS_T>(strs)...);;
} // string_concat()

#endif  // NINJA_STRING_CONCAT_H_
