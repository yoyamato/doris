// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0

#include "vec/functions/function.h"

#include <memory>
#include <string>

#include "common/exception.h"
#include "vec/columns/column_const.h"
#include "vec/columns/column_nullable.h"
#include "vec/columns/column_vector.h"
#include "vec/common/assert_cast.h"
#include "vec/core/block.h"
#include "vec/core/column_with_type_and_name.h"
#include "vec/core/types.h"
#include "vec/data_types/data_type_date.h"
#include "vec/data_types/data_type_date_time.h"
#include "vec/data_types/data_type_number.h"
#include "vec/data_types/data_type_string.h"
#include "vec/functions/function_helpers.h"
#include "vec/functions/simple_function_factory.h"
#include "vec/runtime/vdatetime_value.h"

namespace doris::vectorized {

namespace {

template <TimeUnit unit>
constexpr bool has_time_part() {
    return unit == TimeUnit::HOUR || unit == TimeUnit::MINUTE || unit == TimeUnit::SECOND ||
           unit == TimeUnit::MICROSECOND || unit == TimeUnit::SECOND_MICROSECOND ||
           unit == TimeUnit::MINUTE_MICROSECOND || unit == TimeUnit::MINUTE_SECOND ||
           unit == TimeUnit::HOUR_MICROSECOND || unit == TimeUnit::HOUR_SECOND ||
           unit == TimeUnit::HOUR_MINUTE || unit == TimeUnit::DAY_MICROSECOND ||
           unit == TimeUnit::DAY_SECOND || unit == TimeUnit::DAY_MINUTE || unit == TimeUnit::DAY_HOUR;
}

template <TimeUnit unit, typename DateValueType, typename ResultDateValueType, typename ResultType,
          typename Arg>
ResultType date_time_add_interval(const Arg& t, const TimeInterval& interval, bool is_sub,
                                  bool& is_null) {
    auto ts_value = binary_cast<Arg, DateValueType>(t);
    TimeInterval op = interval;
    if (is_sub) {
        op.is_neg = !op.is_neg;
    }
    if constexpr (std::is_same_v<VecDateTimeValue, DateValueType> ||
                  std::is_same_v<DateValueType, ResultDateValueType>) {
        is_null = !(ts_value.template date_add_interval<unit>(op));
        return binary_cast<ResultDateValueType, ResultType>(ts_value);
    } else {
        ResultDateValueType res;
        is_null = !(ts_value.template date_add_interval<unit>(op, res));
        return binary_cast<ResultDateValueType, ResultType>(res);
    }
}

template <TimeUnit unit, bool is_sub, typename DateType>
class FunctionDateIntervalAddSub final : public IFunction {
public:
    static FunctionPtr create() { return std::make_shared<FunctionDateIntervalAddSub>(); }

    String get_name() const override {
        return is_sub ? (get_unit_name() + "_sub") : (get_unit_name() + "_add");
    }

    size_t get_number_of_arguments() const override { return 2; }
    bool use_default_implementation_for_nulls() const override { return false; }

    DataTypePtr get_return_type_impl(const ColumnsWithTypeAndName& arguments) const override {
        if (!is_date_or_datetime(remove_nullable(arguments[0].type)) &&
            !is_date_v2_or_datetime_v2(remove_nullable(arguments[0].type))) {
            throw Exception(ErrorCode::INVALID_ARGUMENT,
                            "Illegal type {} for first argument of {}",
                            arguments[0].type->get_name(), get_name());
        }
        auto rhs = remove_nullable(arguments[1].type);
        if (!rhs->is_string_or_fixed_string() && !is_integer(remove_nullable(arguments[1].type))) {
            throw Exception(ErrorCode::INVALID_ARGUMENT,
                            "Illegal type {} for second argument of {}",
                            arguments[1].type->get_name(), get_name());
        }
        if constexpr (std::is_same_v<DateType, DataTypeDateV2> && has_time_part<unit>()) {
            return std::make_shared<DataTypeNullable>(std::make_shared<DataTypeDateTimeV2>());
        }
        if constexpr (std::is_same_v<DateType, DataTypeDate>) {
            return std::make_shared<DataTypeNullable>(std::make_shared<DataTypeDateTime>());
        }
        return make_nullable(std::make_shared<DateType>());
    }

    Status execute_impl(FunctionContext*, Block& block, const ColumnNumbers& arguments, size_t result,
                        size_t input_rows_count) const override {
        using FromType = typename DateType::FieldType;
        using ResultDataType = std::conditional_t<
                std::is_same_v<DateType, DataTypeDate>, DataTypeDateTime,
                std::conditional_t<std::is_same_v<DateType, DataTypeDateV2> && has_time_part<unit>(),
                                   DataTypeDateTimeV2, DateType>>;
        using ToType = typename ResultDataType::FieldType;

        const ColumnPtr first = block.get_by_position(arguments[0]).column->convert_to_full_column_if_const();
        const ColumnPtr second = block.get_by_position(arguments[1]).column->convert_to_full_column_if_const();

        const ColumnNullable* first_nullable = check_and_get_column<ColumnNullable>(first.get());
        const ColumnNullable* second_nullable = check_and_get_column<ColumnNullable>(second.get());
        const IColumn* first_nested = first_nullable ? &first_nullable->get_nested_column() : first.get();
        const IColumn* second_nested = second_nullable ? &second_nullable->get_nested_column() : second.get();

        const auto* first_vec = check_and_get_column<ColumnVector<FromType>>(first_nested);
        if (first_vec == nullptr) {
            return Status::RuntimeError("First argument column type mismatch for {}", get_name());
        }

        auto res_col = ColumnVector<ToType>::create();
        auto null_map = ColumnUInt8::create(input_rows_count, 0);

        for (size_t i = 0; i < input_rows_count; ++i) {
            bool is_null = false;
            if ((first_nullable && first_nullable->get_null_map_data()[i]) ||
                (second_nullable && second_nullable->get_null_map_data()[i])) {
                is_null = true;
            }

            TimeInterval interval;
            if (!is_null) {
                std::string expr;
                if (second_nested->is_column_string()) {
                    auto ref = second_nested->get_data_at(i);
                    expr.assign(ref.data, ref.size);
                } else {
                    expr = std::to_string(second_nested->get_int(i));
                }
                if (!parse_mysql_interval(unit, expr, &interval)) {
                    is_null = true;
                }
            }

            ToType out {};
            if (!is_null) {
                if constexpr (std::is_same_v<DateType, DataTypeDate> ||
                              std::is_same_v<DateType, DataTypeDateTime>) {
                    out = date_time_add_interval<unit, doris::VecDateTimeValue,
                                                 doris::VecDateTimeValue,
                                                 ToType>(first_vec->get_data()[i], interval, is_sub, is_null);
                } else if constexpr (std::is_same_v<DateType, DataTypeDateV2>) {
                    if constexpr (has_time_part<unit>()) {
                        out = date_time_add_interval<unit, DateV2Value<DateV2ValueType>,
                                                     DateV2Value<DateTimeV2ValueType>, ToType>(
                                first_vec->get_data()[i], interval, is_sub, is_null);
                    } else {
                        out = date_time_add_interval<unit, DateV2Value<DateV2ValueType>,
                                                     DateV2Value<DateV2ValueType>, ToType>(
                                first_vec->get_data()[i], interval, is_sub, is_null);
                    }
                } else {
                    out = date_time_add_interval<unit, DateV2Value<DateTimeV2ValueType>,
                                                 DateV2Value<DateTimeV2ValueType>, ToType>(
                            first_vec->get_data()[i], interval, is_sub, is_null);
                }
            }
            res_col->get_data().push_back(out);
            null_map->get_data()[i] = is_null;
        }

        block.get_by_position(result).column =
                ColumnNullable::create(std::move(res_col), std::move(null_map));
        return Status::OK();
    }

private:
    static std::string get_unit_name() {
        switch (unit) {
        case TimeUnit::SECOND_MICROSECOND:
            return "second_microsecond";
        case TimeUnit::MINUTE_MICROSECOND:
            return "minute_microsecond";
        case TimeUnit::MINUTE_SECOND:
            return "minute_second";
        case TimeUnit::HOUR_MICROSECOND:
            return "hour_microsecond";
        case TimeUnit::HOUR_SECOND:
            return "hour_second";
        case TimeUnit::HOUR_MINUTE:
            return "hour_minute";
        case TimeUnit::DAY_MICROSECOND:
            return "day_microsecond";
        case TimeUnit::DAY_SECOND:
            return "day_second";
        case TimeUnit::DAY_MINUTE:
            return "day_minute";
        case TimeUnit::DAY_HOUR:
            return "day_hour";
        case TimeUnit::YEAR_MONTH:
            return "year_month";
        default:
            return "";
        }
    }

};

#define REGISTER_UNIT(UNIT)                                                                        \
    factory.register_function<FunctionDateIntervalAddSub<TimeUnit::UNIT, false, DataTypeDate>>(); \
    factory.register_function<FunctionDateIntervalAddSub<TimeUnit::UNIT, false, DataTypeDateTime>>(); \
    factory.register_function<FunctionDateIntervalAddSub<TimeUnit::UNIT, false, DataTypeDateV2>>(); \
    factory.register_function<FunctionDateIntervalAddSub<TimeUnit::UNIT, false, DataTypeDateTimeV2>>(); \
    factory.register_function<FunctionDateIntervalAddSub<TimeUnit::UNIT, true, DataTypeDate>>(); \
    factory.register_function<FunctionDateIntervalAddSub<TimeUnit::UNIT, true, DataTypeDateTime>>(); \
    factory.register_function<FunctionDateIntervalAddSub<TimeUnit::UNIT, true, DataTypeDateV2>>(); \
    factory.register_function<FunctionDateIntervalAddSub<TimeUnit::UNIT, true, DataTypeDateTimeV2>>();

} // namespace

void register_function_date_interval_add_sub(SimpleFunctionFactory& factory) {
    REGISTER_UNIT(SECOND_MICROSECOND)
    REGISTER_UNIT(MINUTE_MICROSECOND)
    REGISTER_UNIT(MINUTE_SECOND)
    REGISTER_UNIT(HOUR_MICROSECOND)
    REGISTER_UNIT(HOUR_SECOND)
    REGISTER_UNIT(HOUR_MINUTE)
    REGISTER_UNIT(DAY_MICROSECOND)
    REGISTER_UNIT(DAY_SECOND)
    REGISTER_UNIT(DAY_MINUTE)
    REGISTER_UNIT(DAY_HOUR)
    REGISTER_UNIT(YEAR_MONTH)
}

} // namespace doris::vectorized
