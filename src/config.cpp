
#include <sstream>
#include <set>

#include <scsi/config.h>
#include <scsi/util.h>

#include "glps_parser.h"

const Config::value_t&
Config::getAny(const std::string& name) const
{
    values_t::const_iterator it=values.find(name);
    if(it==values.end()) throw key_error(name);
    return it->second;
}

void
Config::setAny(const std::string& name, const value_t& val)
{
    values[name] = val;
}

void
Config::swapAny(const std::string& name, value_t& val)
{
    {
        values_t::iterator it = values.find(name);
        if(it!=values.end()) {
            it->second.swap(val);
            return;
        }
    }
    std::pair<values_t::iterator, bool> ret = values.insert(std::make_pair(name,value_t()));
    assert(ret.second);
    ret.first->second.swap(val);
}

namespace {
struct show_value : public boost::static_visitor<void>
{
    unsigned indent;
    std::ostream& strm;
    const std::string& name;
    show_value(std::ostream& s, const std::string& n, unsigned ind=0)
        : indent(ind), strm(s), name(n) {}

    void operator()(double v) const
    {
        unsigned i=indent;
        while(i--)
            strm.put(' ');
        strm << name << " = " << v << "\n";
    }

    void operator()(const std::string& v) const
    {
        doindent();
        strm << name << " = \"" << v << "\"\n";
    }

    void operator()(const std::vector<double>& v) const
    {
        doindent();
        strm << name << " = [";
        for(size_t i=0, N=v.size(); i<N; i++)
        {
            if(i!=0)
                strm << ", ";
            strm << v[i];
        }
        strm << "]\n";
    }

    void operator()(const Config::vector_t& v) const
    {
        doindent();
        strm << name << " = [\n";
        for(size_t i=0, N=v.size(); i<N; i++)
        {
            doindent(2);
            strm << "[" << i << "] = {\n";
            v[i].show(strm, indent+4);
            doindent(2);
            strm << "},\n";
        }
        doindent();
        strm << "]\n";
    }

    void doindent(unsigned extra=0) const
    {
        unsigned i=indent+extra;
        while(i--)
            strm.put(' ');
    }
};
}

void
Config::show(std::ostream& strm, unsigned indent) const
{
    for(Config::values_t::const_iterator it=values.begin(), end=values.end();
        it!=end; ++it)
    {
        boost::apply_visitor(show_value(strm, it->first, indent), it->second);
    }
}

namespace {
// store variable definitions in parser context
struct store_ctxt_var : public boost::static_visitor<void>
{
    const std::string& name;
    parse_context& ctxt;
    store_ctxt_var(parse_context& c, const std::string& n)
        :name(n), ctxt(c)
    {}
#define VISIT(TYPE, E) \
    void operator()(TYPE v) const { \
        ctxt.vars.push_back(parse_var(name.c_str(), expr_t(E, v))); \
        ctxt.var_idx[name] = ctxt.vars.size()-1; \
    }
    VISIT(double, glps_expr_number)
    VISIT(const std::string&, glps_expr_string)
    VISIT(const std::vector<double>&, glps_expr_vector)
#undef VISIT
    void operator()(const Config::vector_t&) const
    {
        // ignore
    }
};

void assign_expr_to_Config(Config& conf, const std::string& name, expr_t& expr)
{
    switch(expr.etype)
    {
    case glps_expr_number:
        conf.set<double>(name, boost::get<double>(expr.value));
        break;
    case glps_expr_string:
        conf.swap<std::string>(name, boost::get<std::string>(expr.value));
        break;
    case glps_expr_vector:
        conf.swap<std::vector<double> >(name, boost::get<std::vector<double> >(expr.value));
        break;
    default:
        throw std::logic_error("Context contained unresolved/illegal variable");
    }
}
}

struct GLPSParser::Pvt {
    typedef Config::values_t values_t;
    values_t vars;

    void fill_vars(parse_context& ctxt)
    {
        for(values_t::const_iterator it=vars.begin(), end=vars.end(); it!=end; ++it)
        {
            // fill in ctxt.vars and ctxt.var_idx
            boost::apply_visitor(store_ctxt_var(ctxt, it->first), it->second);
        }
    }

    Config* fill_context(parse_context& ctxt)
    {
        std::auto_ptr<Config> ret(new Config);
        ret->reserve(ctxt.vars.size()+2);

        // copy ctxt.vars to top level Config
        for(parse_context::vars_t::iterator it=ctxt.vars.begin(), end=ctxt.vars.end();
            it!=end; ++it)
        {
            assign_expr_to_Config(*ret, it->name, it->expr);
        }

        if(ctxt.line.size()==0)
            throw std::runtime_error("No beamlines defined by this file");

        parse_line *line = NULL;

        {
            // find the magic "USE" element.  eg "USE: linename;"
            parse_context::map_idx_t::const_iterator it=ctxt.element_idx.find("USE");
            if(it!=ctxt.element_idx.end()) {
                parse_element &elem = ctxt.elements[it->second];
                parse_context::map_idx_t::const_iterator lit = ctxt.line_idx.find(elem.etype);

                if(lit!=ctxt.line_idx.end()) {
                    line = &ctxt.line[lit->second];
                } else {
                    std::ostringstream strm;
                    strm<<"\"USE: "<<elem.etype<<";\" references undefined beamline";
                    throw std::runtime_error(strm.str());
                }
            } else {
                // no magic USE, default to last line
                line = &ctxt.line.back();
            }
        }

        assert(line);

        if(line->names.size()==0) {
            std::ostringstream strm;
            strm<<"Beamline '"<<line->label<<"' has no elements";
            throw std::runtime_error(strm.str());
        }

        Config::vector_t elements;
        elements.resize(line->names.size());

        // copy in elements
        size_t i = 0;
        for(strlist_t::list_t::const_iterator it=line->names.begin(), end=line->names.end();
            it!=end; ++it)
        {
            Config& next = elements[i++];
            parse_element& elem = ctxt.elements[ctxt.element_idx[*it]];

            next.reserve(elem.props.size()+2);

            // push elements properties
            for(kvlist_t::map_t::iterator itx=elem.props.begin(), endx=elem.props.end();
                itx!=endx; ++itx)
            {
                assign_expr_to_Config(next, itx->first, itx->second);
            }

            // special properties
            next.swap<std::string>("type", elem.etype);
            next.swap<std::string>("name", elem.label);

            elem.props.clear();
        }

        ret->swap<std::string>("name", line->label);
        ret->swap<Config::vector_t>("elements", elements);

        return ret.release();
    }
};

GLPSParser::GLPSParser()
    :priv(new Pvt)
{}

GLPSParser::~GLPSParser() {}

void
GLPSParser::setVar(const std::string& name, const Config::value_t& v)
{
    priv->vars[name] = v;
}

Config*
GLPSParser::parse(FILE *fp)
{
    parse_context ctxt;
    priv->fill_vars(ctxt);
    ctxt.parse(fp);
    return priv->fill_context(ctxt);
}

Config*
GLPSParser::parse(const std::string& s)
{
    parse_context ctxt;
    priv->fill_vars(ctxt);
    ctxt.parse(s);
    return priv->fill_context(ctxt);
}

namespace {
// show the properties of a GLPS element
struct glps_show_props : public boost::static_visitor<void>
{
    std::ostream& strm;
    const std::string& name;
    glps_show_props(std::ostream& s, const std::string& n) :strm(s), name(n) {}

    void operator()(double v) const
    {
        strm<<", "<<name<<" = "<<v;
    }

    void operator()(const std::string& v) const
    {
        strm<<", "<<name<<" = \""<<v<<"\"";
    }

    void operator()(const std::vector<double>& v) const
    {
        strm <<", " << name << " = [";
        for(size_t i=0, N=v.size(); i<N; i++)
        {
            if(i!=0)
                strm << ", ";
            strm << v[i];
        }
        strm << "]";
    }

    void operator()(const Config::vector_t& v) const
    {
        // ignore
    }
};
// show the base GLPS Config (variables and the elements array)
struct glps_show : public boost::static_visitor<void>
{
    std::ostream& strm;
    const std::string& name;
    glps_show(std::ostream& s, const std::string& n) :strm(s), name(n) {}

    void operator()(double v) const
    {
        strm<<name<<" = "<<v<<";\n";
    }

    void operator()(const std::string& v) const
    {
        strm<<name<<" = \""<<v<<"\";\n";
    }

    void operator()(const std::vector<double>& v) const
    {
        strm << name << " = [";
        for(size_t i=0, N=v.size(); i<N; i++)
        {
            if(i!=0)
                strm << ", ";
            strm << v[i];
        }
        strm << "];\n";
    }

    void operator()(const Config::vector_t& v) const
    {
        if(name!="elements") {
            // The GLPS format Only supports nested beamline definitions
            strm << "# "<<name<<" = [... skipped ...];\n";
            return;
        }
    }
};
}

void GLPSPrint(std::ostream& strm, const Config& conf)
{
    // print variables
    for(Config::const_iterator it=conf.begin(), end=conf.end();
        it!=end; ++it)
    {
        boost::apply_visitor(glps_show(strm, it->first), it->second);
    }

    const Config::vector_t *v;
    try{
        v = &conf.get<Config::vector_t>("elements");
    }catch(key_error&){
        strm<<"# Missing beamline element list\n";
        return;
    }catch(boost::bad_get&){
        strm<<"# 'elements' is not a beamline element list\n";
        return;
    }

    std::vector<std::string> line;
    line.reserve(v->size());

    std::set<std::string> eshown;

    // print element definitions
    for(Config::vector_t::const_iterator it=v->begin(), end=v->end();
        it!=end; ++it)
    {
        bool ok = true;
        try {
            const std::string& name=it->get<std::string>("name");
            const std::string& type=it->get<std::string>("type");
            line.push_back(name);
            // only show element definition once
            if(eshown.find(name)!=eshown.end())
                continue;
            strm<<name<<": "<<type;
            eshown.insert(name);
        }catch(key_error&){
            ok=false;
        }catch(boost::bad_get&){
            ok=false;
        }
        if(!ok)
            strm<<"# <malformed element>";

        for(Config::const_iterator itx=it->begin(), endx=it->end();
            itx!=endx; ++itx)
        {
            if(itx->first=="name" || itx->first=="type")
                continue;
            boost::apply_visitor(glps_show_props(strm, itx->first), itx->second);
        }

        strm<<"\n";
    }

    std::string lname(conf.get<std::string>("name", "default"));
    strm<<lname<<": LINE = (";

    bool first=true;
    for(std::vector<std::string>::const_iterator it=line.begin(), end=line.end();
        it!=end; ++it)
    {
        if(!first)
            strm<<", ";
        first = false;
        strm<<*it;
    }

    strm<<");\nUSE: "<<lname<<";\n";
}
