#include "analysis/smt/backends/Z3Backend.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include <z3++.h>

namespace ctrace::stack::analysis::smt
{
    namespace
    {
        struct SymbolEntry
        {
            std::string name;
            std::uint32_t bitWidth = 64;
            z3::expr expr;

            SymbolEntry(std::string symbolName, std::uint32_t width, z3::expr symbolExpr)
                : name(std::move(symbolName))
                , bitWidth(width)
                , expr(std::move(symbolExpr))
            {
            }
        };

        static std::uint32_t normalizeBitWidth(std::uint32_t bitWidth)
        {
            return bitWidth == 0 ? 1 : bitWidth;
        }

        static z3::expr makeBvConstant(z3::context& ctx, std::int64_t value, std::uint32_t bitWidth)
        {
            const std::uint64_t raw = static_cast<std::uint64_t>(value);
            return ctx.bv_val(std::to_string(raw).c_str(), normalizeBitWidth(bitWidth));
        }

        static std::unordered_map<SymbolId, std::uint32_t>
        collectBitWidths(const ConstraintIR& ir)
        {
            std::unordered_map<SymbolId, std::uint32_t> widths;
            for (const ExprNode& node : ir.nodes)
            {
                if (node.kind != ExprKind::Symbol || node.symbol == 0)
                    continue;
                widths.emplace(node.symbol, normalizeBitWidth(node.bitWidth));
            }
            return widths;
        }

        static std::unordered_map<SymbolId, std::string>
        collectNames(const ConstraintIR& ir)
        {
            std::unordered_map<SymbolId, std::string> names;
            for (const SymbolInfo& symbol : ir.symbols)
            {
                if (symbol.id == 0)
                    continue;
                if (!symbol.debugName.empty())
                    names[symbol.id] = symbol.debugName;
            }
            return names;
        }

        static std::optional<z3::expr>
        getOrCreateSymbol(z3::context& ctx, SymbolId id, std::uint32_t bitWidth,
                          std::unordered_map<SymbolId, SymbolEntry>& symbols,
                          const std::unordered_map<SymbolId, std::string>& names, std::string& error)
        {
            if (id == 0)
            {
                error = "invalid symbol id 0";
                return std::nullopt;
            }

            const auto it = symbols.find(id);
            if (it != symbols.end())
            {
                if (it->second.bitWidth != normalizeBitWidth(bitWidth))
                {
                    error = "symbol width mismatch for symbol id " + std::to_string(id);
                    return std::nullopt;
                }
                return it->second.expr;
            }

            auto nameIt = names.find(id);
            std::string symbolName = (nameIt != names.end()) ? nameIt->second : ("sym_" + std::to_string(id));

            const std::uint32_t width = normalizeBitWidth(bitWidth);
            z3::expr symbolExpr = ctx.bv_const(symbolName.c_str(), width);
            auto inserted =
                symbols.emplace(id, SymbolEntry(symbolName, width, std::move(symbolExpr))).first;
            return inserted->second.expr;
        }

        static std::optional<z3::expr>
        buildExpr(ExprId id, const ConstraintIR& ir, z3::context& ctx,
                  std::unordered_map<ExprId, z3::expr>& cache,
                  std::unordered_map<SymbolId, SymbolEntry>& symbols,
                  const std::unordered_map<SymbolId, std::string>& names, std::string& error)
        {
            if (const auto it = cache.find(id); it != cache.end())
                return it->second;

            if (id >= ir.nodes.size())
            {
                error = "expression id out of bounds";
                return std::nullopt;
            }

            const ExprNode& node = ir.nodes[id];
            const std::uint32_t bitWidth = normalizeBitWidth(node.bitWidth);

            auto memoize = [&](z3::expr value) -> std::optional<z3::expr>
            {
                auto [it, inserted] = cache.emplace(id, std::move(value));
                (void) inserted;
                return it->second;
            };

            auto lhs = [&]() -> std::optional<z3::expr>
            { return buildExpr(node.lhs, ir, ctx, cache, symbols, names, error); };

            auto rhs = [&]() -> std::optional<z3::expr>
            { return buildExpr(node.rhs, ir, ctx, cache, symbols, names, error); };

            switch (node.kind)
            {
            case ExprKind::Symbol:
            {
                std::optional<z3::expr> symbol =
                    getOrCreateSymbol(ctx, node.symbol, bitWidth, symbols, names, error);
                if (!symbol)
                    return std::nullopt;
                return memoize(*symbol);
            }
            case ExprKind::Constant:
                return memoize(makeBvConstant(ctx, node.constant, bitWidth));
            case ExprKind::Ite:
            {
                auto cond = lhs();
                auto onTrue = rhs();
                auto onFalse = buildExpr(node.extra, ir, ctx, cache, symbols, names, error);
                if (!cond || !onTrue || !onFalse)
                    return std::nullopt;
                if (!cond->is_bool())
                {
                    error = "ite condition must be boolean";
                    return std::nullopt;
                }
                return memoize(z3::ite(*cond, *onTrue, *onFalse));
            }
            case ExprKind::Add:
            {
                auto l = lhs();
                auto r = rhs();
                if (!l || !r)
                    return std::nullopt;
                return memoize((*l) + (*r));
            }
            case ExprKind::Sub:
            {
                auto l = lhs();
                auto r = rhs();
                if (!l || !r)
                    return std::nullopt;
                return memoize((*l) - (*r));
            }
            case ExprKind::Mul:
            {
                auto l = lhs();
                auto r = rhs();
                if (!l || !r)
                    return std::nullopt;
                return memoize((*l) * (*r));
            }
            case ExprKind::Shl:
            {
                auto l = lhs();
                auto r = rhs();
                if (!l || !r)
                    return std::nullopt;
                return memoize(z3::shl(*l, *r));
            }
            case ExprKind::LShr:
            {
                auto l = lhs();
                auto r = rhs();
                if (!l || !r)
                    return std::nullopt;
                return memoize(z3::lshr(*l, *r));
            }
            case ExprKind::AShr:
            {
                auto l = lhs();
                auto r = rhs();
                if (!l || !r)
                    return std::nullopt;
                return memoize(z3::ashr(*l, *r));
            }
            case ExprKind::Eq:
            {
                auto l = lhs();
                auto r = rhs();
                if (!l || !r)
                    return std::nullopt;
                return memoize((*l) == (*r));
            }
            case ExprKind::Ne:
            {
                auto l = lhs();
                auto r = rhs();
                if (!l || !r)
                    return std::nullopt;
                return memoize((*l) != (*r));
            }
            case ExprKind::Ult:
            {
                auto l = lhs();
                auto r = rhs();
                if (!l || !r)
                    return std::nullopt;
                return memoize(z3::ult(*l, *r));
            }
            case ExprKind::Ule:
            {
                auto l = lhs();
                auto r = rhs();
                if (!l || !r)
                    return std::nullopt;
                return memoize(z3::ule(*l, *r));
            }
            case ExprKind::Ugt:
            {
                auto l = lhs();
                auto r = rhs();
                if (!l || !r)
                    return std::nullopt;
                return memoize(z3::ugt(*l, *r));
            }
            case ExprKind::Uge:
            {
                auto l = lhs();
                auto r = rhs();
                if (!l || !r)
                    return std::nullopt;
                return memoize(z3::uge(*l, *r));
            }
            case ExprKind::Slt:
            {
                auto l = lhs();
                auto r = rhs();
                if (!l || !r)
                    return std::nullopt;
                return memoize(z3::slt(*l, *r));
            }
            case ExprKind::Sle:
            {
                auto l = lhs();
                auto r = rhs();
                if (!l || !r)
                    return std::nullopt;
                return memoize(z3::sle(*l, *r));
            }
            case ExprKind::Sgt:
            {
                auto l = lhs();
                auto r = rhs();
                if (!l || !r)
                    return std::nullopt;
                return memoize(z3::sgt(*l, *r));
            }
            case ExprKind::Sge:
            {
                auto l = lhs();
                auto r = rhs();
                if (!l || !r)
                    return std::nullopt;
                return memoize(z3::sge(*l, *r));
            }
            case ExprKind::And:
            {
                auto l = lhs();
                auto r = rhs();
                if (!l || !r)
                    return std::nullopt;
                return memoize((*l) && (*r));
            }
            case ExprKind::Or:
            {
                auto l = lhs();
                auto r = rhs();
                if (!l || !r)
                    return std::nullopt;
                return memoize((*l) || (*r));
            }
            case ExprKind::Not:
            {
                auto l = lhs();
                if (!l)
                    return std::nullopt;
                return memoize(!(*l));
            }
            case ExprKind::ZExt:
            {
                auto l = lhs();
                if (!l)
                    return std::nullopt;
                const unsigned srcWidth = static_cast<unsigned>(l->get_sort().bv_size());
                if (bitWidth < srcWidth)
                {
                    error = "invalid zext width";
                    return std::nullopt;
                }
                return memoize(z3::zext(*l, bitWidth - srcWidth));
            }
            case ExprKind::SExt:
            {
                auto l = lhs();
                if (!l)
                    return std::nullopt;
                const unsigned srcWidth = static_cast<unsigned>(l->get_sort().bv_size());
                if (bitWidth < srcWidth)
                {
                    error = "invalid sext width";
                    return std::nullopt;
                }
                return memoize(z3::sext(*l, bitWidth - srcWidth));
            }
            case ExprKind::Trunc:
            {
                auto l = lhs();
                if (!l)
                    return std::nullopt;
                const unsigned srcWidth = static_cast<unsigned>(l->get_sort().bv_size());
                if (bitWidth > srcWidth)
                {
                    error = "invalid trunc width";
                    return std::nullopt;
                }
                if (bitWidth == srcWidth)
                    return memoize(*l);
                return memoize(l->extract(bitWidth - 1, 0));
            }
            }

            error = "unsupported expression kind";
            return std::nullopt;
        }
    } // namespace

    std::string Z3Backend::name() const
    {
        return "z3";
    }

    SmtAnswer Z3Backend::solve(const SmtQuery& query) const
    {
        if (query.budgetNodes != 0 && query.ir.nodes.size() > query.budgetNodes)
        {
            return SmtAnswer{
                .status = SmtStatus::Timeout,
                .backendName = name(),
                .reason = std::string("query budget exceeded before solver invocation")};
        }

        try
        {
            z3::context ctx;
            z3::solver solver(ctx);

            if (query.timeoutMs > 0)
            {
                z3::params params(ctx);
                params.set("timeout", query.timeoutMs);
                solver.set(params);
            }

            const auto bitWidths = collectBitWidths(query.ir);
            const auto names = collectNames(query.ir);
            std::unordered_map<SymbolId, SymbolEntry> symbols;

            // Avoid adding duplicated constraints: when symbolic assertions exist,
            // interval bounds are already encoded there.
            const bool useIntervalFallbackOnly = query.ir.assertions.empty();
            if (useIntervalFallbackOnly)
            {
                for (const IntervalConstraint& interval : query.ir.intervals)
                {
                    const std::uint32_t bitWidth =
                        bitWidths.contains(interval.symbol) ? bitWidths.at(interval.symbol) : 64;

                    std::string error;
                    std::optional<z3::expr> symbol =
                        getOrCreateSymbol(ctx, interval.symbol, bitWidth, symbols, names, error);
                    if (!symbol)
                        return SmtAnswer{
                            .status = SmtStatus::Unknown, .backendName = name(), .reason = error};

                    if (interval.hasLower)
                        solver.add(z3::sge(*symbol, makeBvConstant(ctx, interval.lower, bitWidth)));
                    if (interval.hasUpper)
                        solver.add(z3::sle(*symbol, makeBvConstant(ctx, interval.upper, bitWidth)));
                }
            }

            std::unordered_map<ExprId, z3::expr> cache;
            for (ExprId assertionId : query.ir.assertions)
            {
                std::string error;
                std::optional<z3::expr> assertion =
                    buildExpr(assertionId, query.ir, ctx, cache, symbols, names, error);
                if (!assertion)
                {
                    return SmtAnswer{
                        .status = SmtStatus::Unknown, .backendName = name(), .reason = std::move(error)};
                }
                if (!assertion->is_bool())
                {
                    return SmtAnswer{.status = SmtStatus::Unknown,
                                     .backendName = name(),
                                     .reason = std::string("non-boolean assertion")};
                }
                solver.add(*assertion);
            }

            const z3::check_result result = solver.check();
            if (result == z3::sat)
                return SmtAnswer{.status = SmtStatus::Sat, .backendName = name(), .reason = std::nullopt};
            if (result == z3::unsat)
                return SmtAnswer{.status = SmtStatus::Unsat, .backendName = name(), .reason = std::nullopt};

            const std::string reason = solver.reason_unknown();
            if (reason == "timeout")
                return SmtAnswer{.status = SmtStatus::Timeout, .backendName = name(), .reason = reason};
            return SmtAnswer{.status = SmtStatus::Unknown, .backendName = name(), .reason = reason};
        }
        catch (const z3::exception& ex)
        {
            return SmtAnswer{
                .status = SmtStatus::Error, .backendName = name(), .reason = std::string(ex.msg())};
        }
    }
} // namespace ctrace::stack::analysis::smt
