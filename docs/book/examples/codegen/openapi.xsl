<?xml version="1.0" encoding="UTF-8"?>
<!-- Describes the API as an OpenAPI 3.1 document, from the same sources. -->
<xsl:stylesheet version="4.0"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    xmlns:map="http://www.w3.org/2005/xpath-functions/map"
    xmlns:api="https://example.org/api"
    xmlns:f="https://example.org/openapi"
    exclude-result-prefixes="#all">

  <xsl:output method="json" indent="yes" escape-solidus="no"/>

  <xsl:variable name="schema" select="doc(resolve-uri(/api:api/@types, base-uri(/)))"/>

  <!-- The JSON Schema of an XSD type name. -->
  <xsl:function name="f:schema" as="map(*)">
    <xsl:param name="name" as="xs:string"/>
    <xsl:sequence select="{
      'xs:string': { 'type': 'string' },
      'xs:integer': { 'type': 'integer' },
      'xs:decimal': { 'type': 'number' },
      'xs:boolean': { 'type': 'boolean' },
      'xs:date': { 'type': 'string', 'format': 'date' }
    }($name) otherwise { '$ref': '#/components/schemas/' || $name }"/>
  </xsl:function>

  <!-- A JSON body of an XSD type. -->
  <xsl:function name="f:json" as="map(*)">
    <xsl:param name="name" as="xs:string"/>
    <xsl:sequence select="{ 'application/json': { 'schema': f:schema($name) } }"/>
  </xsl:function>

  <xsl:template match="/api:api">
    <xsl:map>
      <xsl:map-entry key="'openapi'" select="'3.1.0'"/>
      <xsl:map-entry key="'info'" select="{ 'title': string(@name), 'version': '1.0.0' }"/>
      <xsl:map-entry key="'paths'">
        <xsl:map>
          <xsl:apply-templates select="api:query | api:command"/>
        </xsl:map>
      </xsl:map-entry>
      <xsl:map-entry key="'components'">
        <xsl:map>
          <xsl:map-entry key="'schemas'">
            <xsl:map>
              <xsl:apply-templates select="$schema/xs:schema/*"/>
            </xsl:map>
          </xsl:map-entry>
        </xsl:map>
      </xsl:map-entry>
    </xsl:map>
  </xsl:template>

  <!-- A query reads its input from the URL, a command from the body.
       The tags name the clients that offer the function. -->
  <xsl:template match="api:query | api:command">
    <xsl:variable name="operation" select="{
      'operationId': string(@name),
      'summary': normalize-space(),
      'tags': array { tokenize(@clients) },
      'responses': {
        '200': { 'description': 'The output', 'content': f:json(@output) },
        '400': { 'description': 'The input is not valid' }
      }
    }"/>
    <xsl:map-entry key="'/api/' || @name" select="
      if (self::api:query)
      then { 'get': map:put($operation, 'parameters', [{
        'name': 'input', 'in': 'query', 'required': true(), 'content': f:json(@input)
      }]) }
      else { 'post': map:put($operation, 'requestBody', {
        'required': true(), 'content': f:json(@input)
      }) }"/>
  </xsl:template>

  <xsl:template match="xs:simpleType">
    <xsl:map-entry key="string(@name)">
      <xsl:map>
        <xsl:sequence select="f:schema(xs:restriction/@base)"/>
        <xsl:if test="xs:restriction/xs:enumeration">
          <xsl:map-entry key="'enum'" select="array { xs:restriction/xs:enumeration/@value ! string() }"/>
        </xsl:if>
        <xsl:apply-templates select="xs:restriction/(* except xs:enumeration)"/>
      </xsl:map>
    </xsl:map-entry>
  </xsl:template>

  <!-- Facets become JSON Schema keywords. XSD patterns are anchored. -->
  <xsl:template match="xs:pattern">
    <xsl:map-entry key="'pattern'" select="'^(?:' || @value || ')$'"/>
  </xsl:template>
  <xsl:template match="xs:minLength | xs:maxLength">
    <xsl:map-entry key="local-name()" select="xs:integer(@value)"/>
  </xsl:template>
  <xsl:template match="xs:minInclusive">
    <xsl:map-entry key="'minimum'" select="xs:decimal(@value)"/>
  </xsl:template>
  <xsl:template match="xs:maxInclusive">
    <xsl:map-entry key="'maximum'" select="xs:decimal(@value)"/>
  </xsl:template>
  <xsl:template match="xs:restriction/*" priority="-1">
    <xsl:message terminate="yes" error-code="f:unsupported"
        select="'No JSON Schema keyword for the facet ' || local-name()"/>
  </xsl:template>

  <xsl:template match="xs:complexType">
    <xsl:variable name="elements" select="xs:sequence/xs:element"/>
    <xsl:map-entry key="string(@name)" select="{
      'type': 'object',
      'properties': map:merge($elements ! map:entry(string(@name),
        if (@maxOccurs = 'unbounded')
        then { 'type': 'array', 'items': f:schema(@type) }
        else f:schema(@type))),
      'required': array { $elements[not(@minOccurs = '0') or @maxOccurs = 'unbounded']/@name ! string() },
      'additionalProperties': false()
    }"/>
  </xsl:template>

</xsl:stylesheet>
