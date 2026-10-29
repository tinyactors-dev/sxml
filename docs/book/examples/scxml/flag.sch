<?xml version="1.0" encoding="UTF-8"?>
<!-- Rules of a Vane machine that its schemas cannot say. The context of
     every rule is a node of the machine; vane.xml is the draft it
     implements, vane-data.xsd the schema of its data. -->
<schema xmlns="http://purl.oclc.org/dsdl/schematron"
        xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
        queryBinding="xslt3" schematronEdition="2025">
  <title>Rules of a Vane machine</title>
  <ns prefix="sc" uri="http://www.w3.org/2005/07/scxml"/>
  <ns prefix="vane" uri="urn:example:vane"/>
  <ns prefix="xs" uri="http://www.w3.org/2001/XMLSchema"/>
  <ns prefix="check" uri="urn:example:check"/>

  <let name="declared" value="//vane:event/@name ! string()"/>
  <let name="implemented" value="//@vane:implements ! tokenize(.)"/>

  <!-- The machine alone, for a sketch that names no requirement yet. -->
  <phase id="machine" when="empty(//@vane:implements)">
    <active pattern="states"/>
    <active pattern="events"/>
    <active pattern="data"/>
  </phase>

  <pattern id="states">
    <!-- Every state a path of transitions leads to from the initial one. -->
    <let name="reachable" value="
      let $states := //(sc:state | sc:final)
      return fold-left(1 to count($states), string(/sc:scxml/@initial),
        fn($reached, $pass) {
          distinct-values(($reached,
            $states[@id = $reached]//sc:transition/@target ! tokenize(.)))
        })"/>
    <rule context="sc:state">
      <assert id="reachable" test="@id = $reachable">No transition leads
        to the state <value-of select="@id"/>.</assert>
    </rule>
  </pattern>

  <pattern id="events">
    <rule context="sc:transition[@event]" visit-each="tokenize(@event)">
      <assert id="declared" test=". = $declared">The event
        <value-of select="."/> is not declared in vane:events.</assert>
    </rule>
    <rule context="vane:event">
      <assert id="used" severity="warning"
              test="@name = (//sc:transition/@event ! tokenize(.),
                             //(sc:send | sc:raise)/@event)">Nothing
        handles, sends or raises the event <value-of select="@name"/>.</assert>
    </rule>
  </pattern>

  <!-- The simple types of vane-data.xsd: each has a constructor function. -->
  <xsl:import-schema namespace="urn:example:vane" schema-location="vane-data.xsd"/>

  <!-- Whether a value is one of a type: a cast that fails is false. -->
  <xsl:function name="check:fits" as="xs:boolean">
    <xsl:param name="value" as="xs:string"/>
    <xsl:param name="type" as="xs:QName"/>
    <xsl:try select="exists(function-lookup($type, 1)($value))">
      <xsl:catch select="false()"/>
    </xsl:try>
  </xsl:function>

  <pattern id="data">
    <let name="schema" value="doc('vane-data.xsd')/xs:schema"/>
    <rule context="vane:event[@vane:payload]">
      <assert id="payload"
              test="resolve-QName(@vane:payload, .)
                    = $schema/xs:element/@name ! QName($schema/@targetNamespace, .)">The
        payload <value-of select="@vane:payload"/> of <value-of select="@name"/>
        is no element of vane-data.xsd.</assert>
    </rule>
    <rule context="sc:data[@vane:type]">
      <let name="type" value="resolve-QName(@vane:type, .)"/>
      <assert id="type" test="exists(function-lookup($type, 1))">The
        type <value-of select="@vane:type"/> of <value-of select="@id"/>
        is no simple type of vane-data.xsd.</assert>
      <assert id="value" test="empty(function-lookup($type, 1))
                               or check:fits(@expr, $type)">The value
        <value-of select="@expr"/> of <value-of select="@id"/> is not a
        <value-of select="@vane:type"/>.</assert>
    </rule>
  </pattern>

  <pattern id="requirements">
    <let name="anchors" value="doc('vane.xml')//t/@anchor ! string()"/>
    <rule context="*[@vane:implements]" visit-each="tokenize(@vane:implements)">
      <assert id="known-requirement" test=". = $anchors">The draft has
        no requirement <value-of select="."/>.</assert>
    </rule>
  </pattern>

  <pattern id="draft">
    <rule context="/sc:scxml"
          visit-each="doc('vane.xml')//t[bcp14 = ('MUST', 'MUST NOT')]/@anchor ! string()">
      <assert id="implemented" severity="warning" test=". = $implemented">Nothing
        in the machine implements the draft's <value-of select="."/>.</assert>
    </rule>
  </pattern>
</schema>
