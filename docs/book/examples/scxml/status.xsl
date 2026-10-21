<!-- The status a flag in a given state answers with, made from the
     machine's data and validated as it is made. -->
<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:sc="http://www.w3.org/2005/07/scxml"
    xmlns:vane="urn:example:vane"
    exclude-result-prefixes="sc">
  <xsl:import-schema namespace="http://www.w3.org/2005/07/scxml"
                     schema-location="scxml.xsd"/>
  <xsl:output method="xml" indent="yes"/>

  <!-- Typed parameters: a value given with param=value is converted to
       the type, and one that is not of it stops the transformation. -->
  <xsl:param name="flag" as="vane:flag-key" select="'new-checkout'"/>
  <xsl:param name="state" as="vane:state" select="'stable'"/>
  <xsl:param name="failures" as="vane:failure-count" select="0"/>

  <!-- Validate the input, so that its values have the schema's types. -->
  <xsl:template match="/">
    <xsl:variable name="machine" as="document-node(schema-element(sc:scxml))">
      <xsl:copy-of select="." validation="strict"/>
    </xsl:variable>
    <xsl:apply-templates select="$machine/sc:scxml"/>
  </xsl:template>

  <!-- The new status is validated against its declaration as it is made. -->
  <xsl:template match="schema-element(sc:scxml)">
    <vane:status xsl:validation="strict">
      <vane:flag>{$flag}</vane:flag>
      <vane:state>{$state}</vane:state>
      <vane:failed-checks>{$failures}</vane:failed-checks>
      <xsl:if test="$state = 'paused'">
        <vane:retry-after>{sc:datamodel/sc:data[@id = 'pauseSeconds']/@expr}</vane:retry-after>
      </xsl:if>
    </vane:status>
  </xsl:template>
</xsl:stylesheet>
