<?xml version="1.0" encoding="UTF-8"?>
<sch:rule xmlns:sch="http://purl.oclc.org/dsdl/schematron" context="ignored">
  <sch:let name="limit" value="2"/>
  <sch:assert test="@n &lt;= $limit">External: at most <sch:value-of select="$limit"/>.</sch:assert>
</sch:rule>
