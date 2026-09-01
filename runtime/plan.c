#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/value.h"

#include "plan.h"

struct PgBatchPlanWriter
{
	char	   *kind;
	int			version;
	List	   *fields;
	bool		finished;
};

struct PgBatchPlanReader
{
	const char *kind;
	List	   *fields;
	List	   *read_names;
};

static List *find_field(List *fields, const char *name);

static bool
string_list_member(List *items, const char *value)
{
	foreach_ptr(char, item, items)
	{
		if (strcmp(item, value) == 0)
			return true;
	}
	return false;
}

static void
check_name(const char *name)
{
	if (name == NULL || name[0] == '\0')
		elog(ERROR, "pg_batch plan field requires a name");
}

static List *
find_field(List *fields, const char *name)
{
	foreach_ptr(List, field, fields)
	{
		if (list_length(field) != 2 || !IsA(linitial(field), String))
			elog(ERROR, "pg_batch received malformed plan data");
		if (strcmp(strVal(linitial(field)), name) == 0)
			return field;
	}
	return NIL;
}

static void
write_field(PgBatchPlanWriter *writer, const char *name, void *value)
{
	check_name(name);
	if (writer == NULL || writer->finished)
		elog(ERROR, "pg_batch plan writer is not active");
	if (find_field(writer->fields, name) != NIL)
		elog(ERROR, "pg_batch plan field \"%s\" is duplicated", name);
	writer->fields = lappend(writer->fields,
		list_make2(makeString(pstrdup(name)), value));
}

PgBatchPlanWriter *
pg_batch_plan_writer_create(const char *kind, int version)
{
	PgBatchPlanWriter *writer;

	if (kind == NULL || kind[0] == '\0' || version <= 0)
		elog(ERROR, "pg_batch plan writer requires a kind and version");
	writer = palloc0_object(PgBatchPlanWriter);
	writer->kind = pstrdup(kind);
	writer->version = version;
	return writer;
}

void
pg_batch_plan_write_int(PgBatchPlanWriter *writer, const char *name,
						int value)
{
	write_field(writer, name, makeInteger(value));
}

void
pg_batch_plan_write_string(PgBatchPlanWriter *writer, const char *name,
						   const char *value)
{
	if (value == NULL)
		elog(ERROR, "pg_batch plan string field \"%s\" is NULL", name);
	write_field(writer, name, makeString(pstrdup(value)));
}

void
pg_batch_plan_write_node(PgBatchPlanWriter *writer, const char *name,
						 const Node *value)
{
	write_field(writer, name, copyObject(value));
}

void
pg_batch_plan_write_list(PgBatchPlanWriter *writer, const char *name,
						 const List *value)
{
	if (value != NIL && !IsA(value, List))
		elog(ERROR, "pg_batch plan field \"%s\" is not a List", name);
	write_field(writer, name, copyObject(value));
}

void
pg_batch_plan_write_int_list(PgBatchPlanWriter *writer, const char *name,
							 const List *value)
{
	if (value != NIL && !IsA(value, IntList))
		elog(ERROR, "pg_batch plan field \"%s\" is not an IntList", name);
	write_field(writer, name, copyObject(value));
}

void
pg_batch_plan_write_bitmap(PgBatchPlanWriter *writer, const char *name,
						   const Bitmapset *value)
{
	List	   *members = NIL;
	int			member = -1;

	while ((member = bms_next_member(value, member)) >= 0)
		members = lappend_int(members, member);
	write_field(writer, name, members);
}

List *
pg_batch_plan_writer_finish(PgBatchPlanWriter *writer)
{
	if (writer == NULL || writer->finished)
		elog(ERROR, "pg_batch plan writer is not active");
	writer->finished = true;
	return list_make3(makeString(pstrdup(writer->kind)),
		makeInteger(writer->version), writer->fields);
}

PgBatchPlanReader *
pg_batch_plan_reader_create(const List *data, const char *kind, int version)
{
	PgBatchPlanReader *reader;
	List	   *fields;

	if (kind == NULL || kind[0] == '\0' || version <= 0)
		elog(ERROR, "pg_batch plan reader requires a kind and version");
	if (data == NIL || !IsA(data, List) || list_length(data) != 3 ||
		!IsA(linitial(data), String) || !IsA(lsecond(data), Integer))
		elog(ERROR, "pg_batch received malformed %s plan data", kind);
	if (strcmp(strVal(linitial(data)), kind) != 0)
		elog(ERROR, "pg_batch expected %s plan data, got %s", kind,
			 strVal(linitial(data)));
	if (intVal(lsecond(data)) != version)
		elog(ERROR, "pg_batch expected %s plan version %d, got %d", kind,
			 version, intVal(lsecond(data)));
	fields = lthird(data);
	if (fields != NIL && !IsA(fields, List))
		elog(ERROR, "pg_batch received malformed %s plan fields", kind);
	foreach_ptr(List, field, fields)
	{
		const char *name;

		if (list_length(field) != 2 || !IsA(linitial(field), String))
			elog(ERROR, "pg_batch received malformed %s plan field", kind);
		name = strVal(linitial(field));
		foreach_ptr(List, other, fields)
		{
			if (other == field)
				break;
			if (strcmp(strVal(linitial(other)), name) == 0)
				elog(ERROR, "pg_batch %s plan field \"%s\" is duplicated",
					 kind, name);
		}
	}
	reader = palloc0_object(PgBatchPlanReader);
	reader->kind = kind;
	reader->fields = fields;
	return reader;
}

static void *
read_field(PgBatchPlanReader *reader, const char *name)
{
	List	   *field;

	check_name(name);
	if (reader == NULL)
		elog(ERROR, "pg_batch plan reader is not active");
	if (string_list_member(reader->read_names, name))
		elog(ERROR, "pg_batch %s plan field \"%s\" was read twice",
			 reader->kind, name);
	field = find_field(reader->fields, name);
	if (field == NIL)
		elog(ERROR, "pg_batch %s plan field \"%s\" is missing",
			 reader->kind, name);
	reader->read_names = lappend(reader->read_names, pstrdup(name));
	return lsecond(field);
}

int
pg_batch_plan_read_int(PgBatchPlanReader *reader, const char *name)
{
	Node	   *value = read_field(reader, name);

	if (!IsA(value, Integer))
		elog(ERROR, "pg_batch %s plan field \"%s\" is not an integer",
			 reader->kind, name);
	return intVal(value);
}

const char *
pg_batch_plan_read_string(PgBatchPlanReader *reader, const char *name)
{
	Node	   *value = read_field(reader, name);

	if (!IsA(value, String))
		elog(ERROR, "pg_batch %s plan field \"%s\" is not a string",
			 reader->kind, name);
	return strVal(value);
}

Node *
pg_batch_plan_read_node(PgBatchPlanReader *reader, const char *name)
{
	return read_field(reader, name);
}

List *
pg_batch_plan_read_list(PgBatchPlanReader *reader, const char *name)
{
	List	   *value = read_field(reader, name);

	if (value != NIL && !IsA(value, List))
		elog(ERROR, "pg_batch %s plan field \"%s\" is not a List",
			 reader->kind, name);
	return value;
}

List *
pg_batch_plan_read_int_list(PgBatchPlanReader *reader, const char *name)
{
	List	   *value = read_field(reader, name);

	if (value != NIL && !IsA(value, IntList))
		elog(ERROR, "pg_batch %s plan field \"%s\" is not an IntList",
			 reader->kind, name);
	return value;
}

Bitmapset *
pg_batch_plan_read_bitmap(PgBatchPlanReader *reader, const char *name)
{
	List	   *members = pg_batch_plan_read_int_list(reader, name);
	Bitmapset  *result = NULL;

	foreach_int(member, members)
		result = bms_add_member(result, member);
	return result;
}

void
pg_batch_plan_reader_finish(PgBatchPlanReader *reader)
{
	if (reader == NULL)
		elog(ERROR, "pg_batch plan reader is not active");
	if (list_length(reader->read_names) != list_length(reader->fields))
	{
		foreach_ptr(List, field, reader->fields)
		{
			const char *name = strVal(linitial(field));

			if (!string_list_member(reader->read_names, name))
				elog(ERROR, "pg_batch %s plan field \"%s\" was not read",
					 reader->kind, name);
		}
	}
}
