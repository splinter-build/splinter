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
 * Executes the provided function, in unspecified order, against each index from the index_sequence
 * in the two tuples. Both tuples must be at least the size of the index_sequence provided.
 *
 * \arg func           -- The function to call on each index, with the first tuple's index as
 *                        the first parameter the second tuple's index as the second parameter.
 * \arg firstTup       -- The first tuple to iterate over. This tuple's contents will become
 *                        the first parameter of the function.
 * \arg secondTup      -- The second tuple to iterate over. This tuple's contents will become
 *                        the second parameter of the function.
 * \arg index_sequence -- The list of indicies to iterate over the two tuples.
 */
template<typename FUNC_T, typename FIRST_TUP_T, typename SECOND_TUP_T, size_t ... INDICIES>
constexpr void pairwise_for_each(FUNC_T       && func,
                                 FIRST_TUP_T  && firstTup,
                                 SECOND_TUP_T && secondTup,
                                 std::index_sequence<INDICIES...>)
{
    (func(std::get<INDICIES>(firstTup), std::get<INDICIES>(secondTup)), ...);
} // pairwise_for_each()


//-----------------------------------------------------------------------------

/**
 * Executes the provided function, in unspecified order, against each index in the two tuples.
 * Both tuples must have identical sizes.
 *
 * \arg func           -- The function to call on each index, with the first tuple's index as
 *                        the first parameter the second tuple's index as the second parameter.
 * \arg firstTup       -- The first tuple to iterate over. This tuple's contents will become
 *                        the first parameter of the function.
 * \arg secondTup      -- The second tuple to iterate over. This tuple's contents will become
 *                        the second parameter of the function.
 */
template<typename FUNC_T, typename FIRST_TUP_T, typename SECOND_TUP_T>
constexpr void pairwise_for_each(FUNC_T       && func,
                                 FIRST_TUP_T  && firstTup,
                                 SECOND_TUP_T && secondTup)
{
    constexpr size_t firstTupSize  = std::tuple_size_v<std::remove_reference_t<FIRST_TUP_T>>;
    constexpr size_t secondTupSize = std::tuple_size_v<std::remove_reference_t<SECOND_TUP_T>>;

    static_assert(firstTupSize == secondTupSize,
                  "pairwise_for_each: First and Second tuples must have exactly the same number of elements.");

    pairwise_for_each(std::forward<FUNC_T>(func),
                      std::forward<FIRST_TUP_T>(firstTup),
                      std::forward<SECOND_TUP_T>(secondTup),
                      std::make_index_sequence<firstTupSize>());
} // pairwise_for_each()

//-----------------------------------------------------------------------------

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
    if constexpr(   std::is_array_v<STRING_T>
                 || std::is_pointer_v<STRING_T>)
    {
        return str;
    }
    else
    {
        return str.data();
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
 * the result of calling the parameters .size() function.
 *
 * If the parameter has no .size() function, a compile error results.
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
        return str.size();
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
    /*
     * Retrieves the raw pointers to the data of each string
     * This uses the C++11 parameter pack expansion syntax
     * and the string_data() function defined above in this file.
     *
     * There's no runtime looping that happens to do this, it's
     * all expanded out at compile time.
     */
    auto const& datas = std::make_tuple(string_data(others)...);

    /*
     * Using the same technique as before, we determine the size of each string
     */
    auto const& sizes = std::make_tuple(string_size(others)...);

    /*
     * Reserve space for all the strings to be stored into the destination.
     *
     * std::apply breaks a tuple into a parameter pack to the provided function.
     * We then use a c++17 fold expression to sum all of the provided values together.
     */
    dest.reserve(string_size(dest) + std::apply([](auto const ... vals){ return (vals + ...); }, sizes));

    /*
     * For each pair of char* / size_t, call the append function on the destination string.
     */
    pairwise_for_each([&dest](auto const data, auto const size){ dest.append(data, size); }, datas, sizes);
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
