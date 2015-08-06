package com.rethinkdb;

import com.rethinkdb.ast.RqlAst;
import com.rethinkdb.response.Backtrace;
import java.util.*;

public class ${camel(classname)} extends ${camel(superclass)} {

    Optional<Backtrace> backtrace = Optional.empty();
    Optional<RqlAst> term = Optional.empty();

    public ${camel(classname)}() {
    }

    public ${camel(classname)}(String message) {
        super(message);
    }

    public ${camel(classname)}(String format, Object... args) {
        super(String.format(format, args));
    }

    public ${camel(classname)}(String message, Throwable cause) {
        super(message, cause);
    }

    public ${camel(classname)}(Throwable cause) {
        super(cause);
    }

    public ${camel(classname)}(String msg, RqlAst term, Backtrace bt) {
        super(msg);
        this.backtrace = Optional.ofNullable(bt);
        this.term = Optional.ofNullable(term);
    }

    public ${camel(classname)} setBacktrace(Backtrace backtrace) {
        this.backtrace = Optional.ofNullable(backtrace);
        return this;
    }

    public Optional<Backtrace> getBacktrace() {
        return backtrace;
    }

    public ${camel(classname)} setTerm(RqlAst term) {
        this.term = Optional.ofNullable(term);
        return this;
    }

    public Optional<RqlAst> getTerm() {
        return this.term;
    }
}